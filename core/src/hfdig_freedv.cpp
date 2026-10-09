// FreeDV decoder of the HF digital receiver (see hfdig_freedv.h), and its test audio for the generator (hfdig_gen.h). The codec2 library
// is loaded at run time (native_common.h); only the few functions of its freedv_api.h that are needed are declared here, from release
// 1.2.0. The decode path has not run against a real library yet (codec2 was not installed where this was written).
#include "dect2/hfdig_freedv.h"
#include "dect2/hfdig_gen.h"
#include "native_common.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

namespace {

// freedv_api.h, codec2 1.2.0
constexpr double kPi = 3.14159265358979323846;
constexpr int FREEDV_MODE_1600 = 0, FREEDV_MODE_700D = 7, FREEDV_MODE_700E = 13;
constexpr int kModeCode[kFreedvModes] = {FREEDV_MODE_700D, FREEDV_MODE_700E, FREEDV_MODE_1600};
const char* const kModeName[kFreedvModes] = {"700D", "700E", "1600"};
// The library's sync flag also fires on RTTY tones, SSTV and noise (FreeDV 1600 above all) and its text channel then gives garbage. A mode
// counts as synced only after the flag held for 1.5 s with an SNR estimate of at least kMinSnrDb; either failing drops it at once.
constexpr uint64_t kConfirmSamples = 12000;
constexpr float kMinSnrDb = 3.f;

struct freedv;   // opaque
using RxTxtCb = void (*)(void*, char);
using TxTxtCb = char (*)(void*);

struct Api {
    native::DynLib lib;
    bool ok = false;
    std::string version;
    freedv* (DECT2_CALL* open)(int) = nullptr;
    void (DECT2_CALL* close)(freedv*) = nullptr;
    int (DECT2_CALL* nin)(freedv*) = nullptr;
    int (DECT2_CALL* rx)(freedv*, short*, short*) = nullptr;
    void (DECT2_CALL* tx)(freedv*, short*, short*) = nullptr;
    void (DECT2_CALL* stats)(freedv*, int*, float*) = nullptr;
    void (DECT2_CALL* setTxt)(freedv*, RxTxtCb, TxTxtCb, void*) = nullptr;
    int (DECT2_CALL* nMaxModem)(freedv*) = nullptr;
    int (DECT2_CALL* nSpeech)(freedv*) = nullptr;
    int (DECT2_CALL* nMaxSpeech)(freedv*) = nullptr;   // optional
    int (DECT2_CALL* nNomModem)(freedv*) = nullptr;
    int (DECT2_CALL* bitErrors)(freedv*) = nullptr;    // optional
    int (DECT2_CALL* totalBits)(freedv*) = nullptr;    // optional
};

// "libcodec2.1.2.dylib" -> "1.2", "libcodec2.so.1.2" -> "1.2", "codec2.dll" -> ""
std::string versionOf(const std::string& path) {
    const size_t sl = path.find_last_of("/\\");
    std::string b = sl == std::string::npos ? path : path.substr(sl + 1);
    size_t i = b.find("codec2");
    if (i == std::string::npos) return std::string();
    i += 6;
    std::string v;
    while (i < b.size()) {
        if (b[i] == '.' && i + 1 < b.size() && isdigit((unsigned char)b[i + 1])) {
            i++;
            while (i < b.size() && (isdigit((unsigned char)b[i]) || (b[i] == '.' && i + 1 < b.size() && isdigit((unsigned char)b[i + 1])))) v += b[i++];
            break;
        }
        i++;
    }
    return v;
}

const Api& api() {
    static const Api a = [] {
        Api r;
        std::vector<std::string> names;
        if (const char* e = getenv("DECT2_CODEC2_PATH")) if (*e) names.push_back(e);
        if (names.empty()) names = native::libNames("codec2", {".1.2", ".1.1", ".1.0", ".1", ".0.9"}, {"codec2.dll", "libcodec2.dll"});
        if (!r.lib.open(names)) return r;
        native::DynLib& l = r.lib;
        const bool ok = l.get(r.open, "freedv_open") && l.get(r.close, "freedv_close") && l.get(r.nin, "freedv_nin") && l.get(r.rx, "freedv_rx") &&
                        l.get(r.tx, "freedv_tx") && l.get(r.stats, "freedv_get_modem_stats") && l.get(r.setTxt, "freedv_set_callback_txt") &&
                        l.get(r.nMaxModem, "freedv_get_n_max_modem_samples") && l.get(r.nSpeech, "freedv_get_n_speech_samples") &&
                        l.get(r.nNomModem, "freedv_get_n_nom_modem_samples");
        l.opt(r.nMaxSpeech, "freedv_get_n_max_speech_samples");
        l.opt(r.bitErrors, "freedv_get_total_bit_errors");
        l.opt(r.totalBits, "freedv_get_total_bits");
        if (!ok) return r;
        r.ok = true;
#ifndef _WIN32
        // the real file behind a bare name, for the report
        Dl_info info;
        if (dlsym(l.h, "freedv_open") && dladdr(reinterpret_cast<void*>(r.open), &info) && info.dli_fname) l.path = info.dli_fname;
#endif
        r.version = versionOf(l.path);
        return r;
    }();
    return a;
}

std::atomic<int> gModeChoice{0};

} // namespace

const char* freedvModeName(int idx) { return idx >= 0 && idx < kFreedvModes ? kModeName[idx] : ""; }
void hfdigFreedvSetMode(int choice) { gModeChoice = std::min(std::max(choice, 0), kFreedvModes); }
int hfdigFreedvMode() { return gModeChoice; }
bool hfdigFreedvLibrary(std::string* path, std::string* version) {
    const Api& a = api();
    if (a.ok) {
        if (path) *path = a.lib.path;
        if (version) *version = a.version;
    }
    return a.ok;
}

// ------------------------------------------------------------------ receiver

namespace {

struct Rx {
    freedv* f = nullptr;
    std::vector<short> acc;      // audio waiting for the next freedv_rx() (from head)
    size_t head = 0;
    std::vector<short> modemIn, speech;
    std::string line;            // text being received
    std::string lastLine;
    bool sync = false;           // confirmed: the library's sync has held with a sane SNR for kConfirmSamples
    float snr = 0;
    uint64_t syncRun = 0;        // samples the library's sync has held with the SNR above the floor
    uint64_t lostSamples = 0;
    int nSpeechMax = 0;
};

void rxChar(void* st, char c) {
    Rx* r = static_cast<Rx*>(st);
    if (c == '\r' || c == '\n' || c == 0) {
        if (!r->line.empty()) { r->lastLine = r->line; r->line.clear(); }
    } else if ((unsigned char)c >= 32 && (unsigned char)c < 127) {
        if (r->line.size() >= 80) { r->lastLine = r->line; r->line.clear(); }
        r->line += c;
    }
}

} // namespace

struct HfdigFreedv::Impl {
    HfdigFreedvTelemetry tel;
    std::function<void(const float*, size_t)> speech;   // setSpeechOut(): where decoded speech goes
    Rx rx[kFreedvModes];
    int active = -1;
    int lastChoice = 0;
    std::vector<float> fl;

    Impl() { openAll(); }
    ~Impl() { closeAll(); }
    void closeAll() {
        const Api& a = api();
        for (Rx& r : rx) {
            if (r.f && a.ok) a.close(r.f);
            r = Rx();
        }
    }
    void openAll() {
        const Api& a = api();
        tel = HfdigFreedvTelemetry();
        tel.libFound = a.ok;
        if (!a.ok) return;
        tel.libPath = a.lib.path;
        tel.libVersion = a.version;
        tel.state = 1;
        for (int m = 0; m < kFreedvModes; m++) {
            Rx& r = rx[m];
            r.f = a.open(kModeCode[m]);
            if (!r.f) continue;
            tel.open[m] = true;
            a.setTxt(r.f, rxChar, nullptr, &r);
            const int nmax = std::max(a.nMaxModem(r.f), 1);
            r.modemIn.assign((size_t)nmax + 16, 0);
            r.nSpeechMax = a.nMaxSpeech ? a.nMaxSpeech(r.f) : 0;
            if (r.nSpeechMax <= 0) r.nSpeechMax = std::max(a.nSpeech(r.f) * 4, 2048);
            r.speech.assign((size_t)r.nSpeechMax + 16, 0);
        }
        active = -1;
        lastChoice = gModeChoice;
    }
};

HfdigFreedv::HfdigFreedv() : p_(std::make_unique<Impl>()) {}
HfdigFreedv::~HfdigFreedv() = default;
void HfdigFreedv::reset() {
    p_->closeAll();
    p_->openAll();
}
void HfdigFreedv::telemetry(HfdigFreedvTelemetry& out) const { out = p_->tel; out.modeChoice = gModeChoice; }
void HfdigFreedv::setSpeechOut(std::function<void(const float* x, size_t n)> out) { p_->speech = std::move(out); }

void HfdigFreedv::feedAudio(const float* x, size_t n) {
    Impl& s = *p_;
    s.tel.audioSamples += n;
    const Api& a = api();
    if (!a.ok) return;
    const int choice = gModeChoice;
    if (choice != s.lastChoice) {   // the user picked another mode: start clean
        s.lastChoice = choice;
        s.active = -1;
        for (Rx& r : s.rx) { r.acc.clear(); r.head = 0; r.sync = false; r.syncRun = 0; r.lostSamples = 0; r.line.clear(); r.lastLine.clear(); }
    }
    s.tel.modeChoice = choice;
    for (int m = 0; m < kFreedvModes; m++) {
        Rx& r = s.rx[m];
        if (!r.f || (choice > 0 && choice - 1 != m)) { r.sync = false; r.syncRun = 0; continue; }
        for (size_t i = 0; i < n; i++) {
            const float v = std::min(std::max(x[i], -1.f), 1.f);
            r.acc.push_back((short)lrintf(v * 12000.f));
        }
        for (;;) {
            const int nin = a.nin(r.f);
            if (nin <= 0 || nin > (int)r.modemIn.size() || r.acc.size() - r.head < (size_t)nin) break;
            std::copy(r.acc.begin() + (long)r.head, r.acc.begin() + (long)r.head + nin, r.modemIn.begin());
            r.head += (size_t)nin;
            const int nout = a.rx(r.f, r.speech.data(), r.modemIn.data());
            int sync = 0;
            float snr = 0;
            a.stats(r.f, &sync, &snr);
            r.snr = std::isfinite(snr) ? snr : 0.f;
            if (sync != 0 && r.snr >= kMinSnrDb) {
                r.syncRun += (uint64_t)nin;
                if (!r.sync && r.syncRun >= kConfirmSamples) { r.sync = true; r.line.clear(); r.lastLine.clear(); }   // the text so far was noise
            } else {
                r.syncRun = 0;
                r.sync = false;
                r.line.clear(); r.lastLine.clear();
            }
            if (!r.sync) r.lostSamples += (uint64_t)nin; else r.lostSamples = 0;
            if (s.active < 0 && r.sync) s.active = m;
            if (m == s.active && r.sync && nout > 0 && nout <= r.nSpeechMax) {
                s.tel.speechFrames++;
                if (s.speech) {
                    s.fl.resize((size_t)nout);
                    for (int i = 0; i < nout; i++) s.fl[(size_t)i] = (float)r.speech[(size_t)i] / 32768.f;
                    s.speech(s.fl.data(), (size_t)nout);
                }
            }
        }
        if (r.head > 4096) { r.acc.erase(r.acc.begin(), r.acc.begin() + (long)r.head); r.head = 0; }
    }
    // keep the active mode while it has sync; let go 2 s after it lost it
    if (s.active >= 0 && !s.rx[s.active].sync && s.rx[s.active].lostSamples > 16000) s.active = -1;
    if (s.active < 0) for (int m = 0; m < kFreedvModes; m++) if (s.rx[m].sync) { s.active = m; break; }
    HfdigFreedvTelemetry& t = s.tel;
    for (int m = 0; m < kFreedvModes; m++) { t.sync[m] = s.rx[m].sync; t.snrDb[m] = s.rx[m].sync ? s.rx[m].snr : 0.f; }
    t.mode = s.active;
    t.state = s.active >= 0 ? 2 : 1;
    if (s.active >= 0) {
        const Rx& r = s.rx[s.active];
        t.snr = r.snr;
        t.text = r.line.empty() ? r.lastLine : r.line;
        if (a.bitErrors && a.totalBits) { t.bitErrors = (uint64_t)std::max(a.bitErrors(r.f), 0); t.bits = (uint64_t)std::max(a.totalBits(r.f), 0); }
    } else {
        t.snr = 0;
        t.text.clear();
    }
}

std::unique_ptr<HfdigFreedv> makeFreedvDecoder() { return std::make_unique<HfdigFreedv>(); }

// ------------------------------------------------------------------ test audio

namespace {

struct TxText { const char* s = "ONAIR TEST\r"; size_t i = 0; };
char txChar(void* st) {
    TxText* t = static_cast<TxText*>(st);
    const char c = t->s[t->i++];
    if (!t->s[t->i]) t->i = 0;
    return c;
}

class FreedvTestAudio : public HfdigTestAudio {
public:
    FreedvTestAudio(const Api& a, freedv* f) : a_(a), f_(f) {
        a_.setTxt(f_, nullptr, txChar, &txt_);
        nSpeech_ = std::max(a_.nSpeech(f_), 1);
        nModem_ = std::max(a_.nNomModem(f_), 1);
        speech_.assign((size_t)nSpeech_, 0);
        modem_.assign((size_t)std::max(a_.nMaxModem(f_), nModem_) + 16, 0);
        // the level of this mode's modem signal sets the gain that brings the audio to an rms of 0.35 (the generator's reference)
        double p = 0;
        size_t cnt = 0;
        for (int fr = 0; fr < 8; fr++) {
            block();
            std::vector<float> v((size_t)nModem_);
            for (int i = 0; i < nModem_; i++) v[(size_t)i] = (float)modem_[(size_t)i];
            if (fr >= 2) for (float s : v) { p += (double)s * s; cnt++; }
            warm_.insert(warm_.end(), v.begin(), v.end());
        }
        gain_ = cnt && p > 0 ? (float)(0.35 / std::sqrt(p / (double)cnt)) : 1.f / 16384.f;
        for (float& s : warm_) s *= gain_;
    }
    ~FreedvTestAudio() override { a_.close(f_); }
    void generate(float* out, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            if (pos_ >= q_.size()) {
                q_.clear(); pos_ = 0;
                if (!warm_.empty()) { q_.swap(warm_); }
                else { block(); for (int k = 0; k < nModem_; k++) q_.push_back((float)modem_[(size_t)k] * gain_); }
            }
            out[i] = std::min(std::max(q_[pos_++], -1.f), 1.f);
        }
    }

private:
    void block() {   // one frame of synthetic speech (voiced, gliding pitch, vowel-like formants, syllable rhythm) through the modulator
        for (int i = 0; i < nSpeech_; i++, t_ += 1.0 / 8000.0) {
            const double f0 = 115.0 + 30.0 * std::sin(2 * kPi * 0.6 * t_);
            ph_ += 2 * kPi * f0 / 8000.0;
            if (ph_ > 2 * kPi) ph_ -= 2 * kPi;
            const double f1 = 500.0 + 300.0 * std::sin(2 * kPi * 0.9 * t_), f2 = 1500.0 + 500.0 * std::sin(2 * kPi * 0.45 * t_ + 1);
            double v = 0;
            for (int k = 1; k * f0 < 3600.0; k++) {
                const double fk = k * f0;
                const double w = std::exp(-std::pow((fk - f1) / 250.0, 2)) + 0.6 * std::exp(-std::pow((fk - f2) / 350.0, 2)) + 0.05;
                v += w * std::sin(k * ph_);
            }
            const double env = std::max(0.0, 0.5 + 0.6 * std::sin(2 * kPi * 3.2 * t_));
            speech_[(size_t)i] = (short)lrint(std::min(std::max(v * env * 2500.0, -20000.0), 20000.0));
        }
        a_.tx(f_, modem_.data(), speech_.data());
    }
    const Api& a_;
    freedv* f_;
    TxText txt_;
    int nSpeech_ = 1, nModem_ = 1;
    std::vector<short> speech_, modem_;
    std::vector<float> q_, warm_;
    size_t pos_ = 0;
    float gain_ = 1.f;
    double t_ = 0, ph_ = 0;
};

} // namespace

std::unique_ptr<HfdigTestAudio> makeFreedvTestAudio(const SynthConfig& cfg) {
    const Api& a = api();
    if (!a.ok) return nullptr;   // no library, no test signal: the shell keeps its noise
    const int m = std::min(std::max(cfg.modeOpt[1], 0), kFreedvModes - 1);
    freedv* f = a.open(kModeCode[m]);
    if (!f) return nullptr;
    return std::make_unique<FreedvTestAudio>(a, f);
}

} // namespace dect2
