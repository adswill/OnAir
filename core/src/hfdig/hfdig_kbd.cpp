// The keyboard-to-keyboard decoder of the HF digital receiver (hfdig_kbd.h): holds the settings, finds the signal in the audio while none
// is locked, runs the engine of the family (hfdig_kbd_<family>.cpp), collects its text and picture and looks for callsigns.
#include "hfdig_kbd_int.h"
#include <atomic>
#include <cctype>
#include <cstring>
#include <chrono>
#include <future>
#include <mutex>

namespace dect2 {

const int kKbdTonesList[8] = {2, 4, 8, 16, 32, 64, 128, 256};
const int kKbdBwList[5] = {125, 250, 500, 1000, 2000};

namespace {
const char* const kFamilyNames[kKbdFamilies] = {"Olivia", "Contestia", "MFSK", "THOR", "DominoEX", "Hellschreiber"};
const char* const kMfskNames[] = {"MFSK4", "MFSK8", "MFSK11", "MFSK16", "MFSK22", "MFSK31", "MFSK32", "MFSK64", "MFSK128", "MFSK64L", "MFSK128L"};
const char* const kThorNames[] = {"THOR 4", "THOR 5", "THOR 8", "THOR 11", "THOR 16", "THOR 22", "THOR 25x4", "THOR 50x1", "THOR 50x2", "THOR 100"};
const char* const kDomNames[] = {"DominoEX 4", "DominoEX 5", "DominoEX 8", "DominoEX 11", "DominoEX 16", "DominoEX 22"};
const char* const kHellNames[] = {"Feld Hell", "Slow Hell", "Hell 80", "HellX5", "HellX9", "FSK Hell 245", "FSK Hell 105"};
}

const char* kbdFamilyName(int f) { return f >= 0 && f < kKbdFamilies ? kFamilyNames[f] : "?"; }
int kbdVariantCount(int f) {
    switch (f) {
    case KbdMfsk: return (int)(sizeof kMfskNames / sizeof *kMfskNames);
    case KbdThor: return (int)(sizeof kThorNames / sizeof *kThorNames);
    case KbdDominoex: return (int)(sizeof kDomNames / sizeof *kDomNames);
    case KbdHell: return (int)(sizeof kHellNames / sizeof *kHellNames);
    default: return 0;
    }
}
const char* kbdVariantName(int f, int v) {
    if (v < 0 || v >= kbdVariantCount(f)) return "";
    switch (f) {
    case KbdMfsk: return kMfskNames[v];
    case KbdThor: return kThorNames[v];
    case KbdDominoex: return kDomNames[v];
    default: return kHellNames[v];
    }
}
int kbdDefaultVariant(int f) { return f == KbdMfsk ? 3 : f == KbdThor ? 4 : f == KbdDominoex ? 3 : 0; }   // MFSK16, THOR 16, DominoEX 11, Feld Hell

double kbdBandwidthHz(const KbdConfig& c) {
    KbdConfig d = c;
    if (d.variant < 0 || d.variant >= kbdVariantCount(d.family)) d.variant = kbdDefaultVariant(d.family);
    std::unique_ptr<kbd::Engine> g;
    switch (d.family) {
    case KbdOlivia: case KbdContestia: g = kbd::makeOliviaEngine(d); break;
    case KbdMfsk: g = kbd::makeMfskEngine(d); break;
    case KbdThor: case KbdDominoex: g = kbd::makeIfkEngine(d); break;
    default: g = kbd::makeHellEngine(d); break;
    }
    return g ? g->bandwidth() : 500;
}

std::string kbdFindCallsign(const std::string& t) {
    // words of letters and digits; a callsign has one to two prefix characters with a letter, a digit, and one to three letters
    std::string best;
    size_t i = 0;
    while (i < t.size()) {
        while (i < t.size() && !std::isalnum((unsigned char)t[i])) i++;
        size_t j = i;
        while (j < t.size() && (std::isalnum((unsigned char)t[j]) || t[j] == '/')) j++;
        const std::string w = t.substr(i, j - i);
        i = j;
        if (w.size() < 3 || w.size() > 10) continue;
        std::string core = w;
        const size_t sl = w.find('/');
        if (sl != std::string::npos) core = sl >= w.size() - sl - 1 ? w.substr(0, sl) : w.substr(sl + 1);
        if (core.size() < 3 || core.size() > 7) continue;
        // upper case only
        bool up = true;
        for (char c : core) if (std::islower((unsigned char)c)) up = false;
        if (!up) continue;
        size_t d = std::string::npos;   // the last digit that has a letter after it
        for (size_t k = 1; k + 1 < core.size(); k++) if (std::isdigit((unsigned char)core[k]) && std::isalpha((unsigned char)core[k + 1])) d = k;
        if (d == std::string::npos) continue;
        bool ok = d <= 2;
        for (size_t k = d + 1; k < core.size(); k++) if (!std::isalpha((unsigned char)core[k])) ok = false;
        const size_t suf = core.size() - d - 1;
        if (!ok || suf < 1 || suf > 3) continue;
        bool letter = false;
        for (size_t k = 0; k < d; k++) { if (std::isalpha((unsigned char)core[k])) letter = true; }
        if (!letter && d == 1) continue;
        // not words such as "K9" ... : prefix must have a letter or be digit-letter (like 9A, 4X)
        best = w;
    }
    return best;
}

struct HfdigKbd::Impl {
    mutable std::mutex mu;
    KbdConfig cfg;
    bool cfgNew = true;
    bool clearReq = false;
    std::function<void(const std::string&)> log;
    std::unique_ptr<kbd::Engine> eng;
    double centre = 1500;
    HfdigKbdTelemetry t;
    uint64_t samples = 0;
    // the signal search
    std::vector<float> win, pow;      // 4096 sample window of the audio, averaged power spectrum
    int winN = 0, evals = 0;
    double cand = 0;
    int stable = 0;
    kbd::Fft fft{4096};
    std::vector<kbd::cf> fb;
    uint64_t lastSearch = 0;
    std::string lastCall;
    // Auto (cfg.autoParams): the last audio, the running analysis, what it found
    std::vector<float> autoHist;
    size_t autoPos = 0, autoFill = 0;
    uint64_t autoLastRun = 0, autoCharsAt = 0, autoCharsSeen = 0, autoHellSeen = 0;
    bool autoLocked = false, autoHave = false;
    int autoTones = 32, autoBw = 1000, autoVar = -1;
    std::future<std::vector<KbdAutoHyp>> autoJob;
    bool autoDiscard = false;
    void autoTick(const float* x, size_t n);
    const KbdPicture* lastPic = nullptr;
    uint64_t picId = 0;
    std::string recent;               // the last characters decoded: do they read as text
    // the Hell picture
    std::deque<uint8_t> hellCols;

    Impl() { win.assign(4096, 0.f); pow.assign(2048, 0.f); fb.assign(4096, kbd::cf()); }

    void build() {
        KbdConfig c = cfg;
        if (c.autoParams && autoHave) { c.tones = autoTones; c.bandwidth = autoBw; if (autoVar >= 0) c.variant = autoVar; }
        if (c.family < 0 || c.family >= kKbdFamilies) c.family = KbdOlivia;
        if (c.variant < 0 || c.variant >= kbdVariantCount(c.family)) c.variant = kbdDefaultVariant(c.family);
        switch (c.family) {
        case KbdOlivia: case KbdContestia: eng = kbd::makeOliviaEngine(c); break;
        case KbdMfsk: eng = kbd::makeMfskEngine(c); break;
        case KbdThor: case KbdDominoex: eng = kbd::makeIfkEngine(c); break;
        default: eng = kbd::makeHellEngine(c); break;
        }
        centre = c.centreHz;
        if (eng) eng->setCentre(centre);
        winN = 0; evals = 0; stable = 0; cand = 0; recent.clear(); lastPic = nullptr;
        std::fill(pow.begin(), pow.end(), 0.f);
        if (!cfg.autoParams) cfg.variant = c.variant;
        cfgNew = false;
    }

    // the strongest stretch of the audio spectrum as wide as the signal: its middle
    void search(uint64_t nowSamples) {
        (void)nowSamples;
        if (!eng) return;
        const double bw = std::max(60.0, eng->bandwidth());
        const double bin = kbd::kFs / 4096;
        const int w = std::max(4, (int)(bw / bin));
        const int lo = (int)(250 / bin), hi = (int)(3450 / bin);
        if (hi - lo <= w + 4) return;
        std::vector<double> acc(hi - lo + 1, 0.0);
        double run = 0;
        for (int i = lo; i < lo + w; i++) run += pow[i];
        std::vector<double> sums;
        int bestI = lo;
        double best = -1;
        for (int i = lo; i + w <= hi; i++) {
            if (i > lo) run += pow[i + w - 1] - pow[i - 1];
            sums.push_back(run);
            if (run > best) { best = run; bestI = i; }
        }
        std::vector<double> s2 = sums;
        std::nth_element(s2.begin(), s2.begin() + s2.size() / 2, s2.end());
        const double med = s2[s2.size() / 2];
        if (!(best > med * 1.15)) { stable = 0; return; }
        const double c = (bestI + w / 2.0) * bin;
        if (std::fabs(c - cand) < std::max(8.0, bw / 16)) stable++; else { cand = c; stable = 1; }
        if (stable >= 3 && std::fabs(cand - centre) > std::max(10.0, bw / 12)) {
            centre = cand;
            eng->setCentre(centre);
            stable = 0;
        }
    }

    // the text of a real signal is letters, digits, spaces and a little punctuation; a decoder next to the signal gives noise
    bool plausible() const {
        if (recent.size() < 12) return false;
        int good = 0;
        for (char c : recent) if (std::isalnum((unsigned char)c) || c == ' ' || c == '.' || c == ',' || c == '\n' || c == '/' || c == '-' || c == '?' || c == '\'' || c == ':') good++;
        return good * 10 >= (int)recent.size() * 9;
    }

    void feed(const float* x, size_t n) {
        std::unique_lock<std::mutex> lk(mu);
        if (cfgNew || !eng) build();
        if (clearReq) { t.text.clear(); t.chars = 0; t.callsign.clear(); hellCols.clear(); t.hellTotal = 0; clearReq = false; }
        const bool autoC = cfg.autoCentre;
        lk.unlock();
        samples += n;
        size_t i = 0;
        while (i < n) {
            const size_t m = std::min(n - i, (size_t)(4096 - winN));
            std::memcpy(&win[winN], x + i, m * sizeof(float));
            winN += (int)m; i += m;
            if (winN == 4096) {
                winN = 0;
                for (int k = 0; k < 4096; k++) fb[k] = kbd::cf((float)(win[k] * (0.5 - 0.5 * std::cos(2 * kbd::kPi * k / 4096))), 0);
                fft.run(fb.data());
                for (int k = 0; k < 2048; k++) pow[k] += (std::norm(fb[k]) - pow[k]) * 0.15f;
                if (autoC && !eng->busy && !(eng->locked && plausible()) && ++evals >= 2) { evals = 0; search(samples); }
            }
        }
        eng->feed(x, n);
        autoTick(x, n);
        lk.lock();
        if (eng->pic && (eng->picDirty || eng->picDone)) {
            if (eng->pic.get() != lastPic) { lastPic = eng->pic.get(); picId++; }
            auto snap = std::make_shared<KbdPicture>(*eng->pic);
            snap->id = picId;
            t.picLive = snap;
            t.picSerial++;
            if (eng->picDone) {
                t.picHistory.insert(t.picHistory.begin(), snap);
                if (t.picHistory.size() > (size_t)kKbdPicHistory) t.picHistory.resize(kKbdPicHistory);
                if (log) log(std::string(kbdVariantName(KbdMfsk, cfg.variant)) + ": picture " + std::to_string(snap->width) + "x" + std::to_string(snap->height) + (snap->colour ? " colour" : " grey"));
            }
            eng->picDirty = eng->picDone = false;
        }
        std::string add;
        add.swap(eng->out);
        if (!add.empty()) {
            for (char c : add) {
                if (c == '\b') { if (!t.text.empty()) t.text.pop_back(); continue; }
                t.text += c;
            }
            t.chars += add.size();
            recent += add;
            if (recent.size() > 40) recent.erase(0, recent.size() - 40);
            if (t.text.size() > (size_t)kKbdTextMax) t.text.erase(0, t.text.size() - kKbdTextMax);
            const size_t from = t.text.size() > 160 ? t.text.size() - 160 : 0;
            const std::string call = kbdFindCallsign(t.text.substr(from));
            if (!call.empty() && call != lastCall) {
                lastCall = call;
                t.callsign = call;
                if (log) log(std::string(kbdFamilyName(cfg.family)) + ": callsign " + call);
            }
        }
        if (!eng->hell.empty()) {
            for (uint8_t v : eng->hell) hellCols.push_back(v);
            t.hellTotal += eng->hell.size() / kKbdHellRows;
            eng->hell.clear();
            const size_t cap = (size_t)kKbdHellMaxCols * kKbdHellRows;
            while (hellCols.size() > cap) hellCols.erase(hellCols.begin(), hellCols.begin() + kKbdHellRows);
        }
        t.state = eng->locked ? 1 : 0;
        t.audioSamples = samples;
        t.cfg = cfg;
        t.centreHz = centre;
        t.offsetHz = eng->offsetHz;
        t.snr = eng->snrDb;
    }
};

HfdigKbd::HfdigKbd() : p_(new Impl) {}
HfdigKbd::~HfdigKbd() = default;
void HfdigKbd::reset() { std::lock_guard<std::mutex> lk(p_->mu); p_->cfgNew = true; p_->t.state = 0; }
void HfdigKbd::feedAudio(const float* x, size_t n) { p_->feed(x, n); }
void HfdigKbd::telemetry(HfdigKbdTelemetry& out) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    out = p_->t;
    out.cfg = p_->cfg;
    out.hellCols = (int)(p_->hellCols.size() / kKbdHellRows);
    out.autoOn = p_->cfg.autoParams; out.autoLocked = p_->autoLocked; out.autoTones = p_->autoTones; out.autoBandwidth = p_->autoBw; out.autoVariant = p_->autoVar;
    out.hell.assign(p_->hellCols.begin(), p_->hellCols.end());
}
// The Auto rule (as RTTY's): no text for a while means the settings are wrong or the signal changed; then the audio goes through the trial decoders on another
// thread. A good decode is left alone. The analysis job is dropped when the mode changes.
void HfdigKbd::Impl::autoTick(const float* x, size_t n) {
    constexpr size_t kCap = (size_t)(30 * kbd::kFs);
    const bool on = cfg.autoParams;
    if (!on) { autoFill = autoPos = 0; autoLocked = false; if (autoJob.valid()) autoDiscard = true; else return; }
    if (autoHist.size() != kCap) autoHist.assign(kCap, 0.f);
    for (size_t i = 0; i < n; i++) { autoHist[autoPos] = x[i]; if (++autoPos >= kCap) autoPos = 0; }
    autoFill = std::min(kCap, autoFill + n);
    if (autoJob.valid()) {
        if (autoJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        auto r = autoJob.get();
        const bool drop = autoDiscard || !on;
        autoDiscard = false;
        if (drop || r.empty()) return;
        const auto& h = r[0];
        autoTones = h.tones > 0 ? h.tones : autoTones;
        autoBw = h.bandwidth > 0 ? h.bandwidth : autoBw;
        autoVar = h.variant;
        autoHave = true; autoLocked = true;
        autoCharsAt = samples; autoCharsSeen = t.chars; autoHellSeen = t.hellTotal;
        std::lock_guard<std::mutex> lk(mu);
        cfgNew = true;
        return;
    }
    const bool hell = cfg.family == KbdHell;
    if (t.chars != autoCharsSeen || (hell && eng && eng->locked)) { autoCharsSeen = t.chars; autoCharsAt = samples; }
    const uint64_t quiet = samples - autoCharsAt;
    if (autoLocked && quiet < (uint64_t)(hell ? 6 : 14) * (uint64_t)kbd::kFs) return;
    autoLocked = false;
    if (autoFill < (size_t)(8 * kbd::kFs) || samples - autoLastRun < (uint64_t)(6 * kbd::kFs)) return;
    autoLastRun = samples;
    std::vector<float> a(autoFill);
    const size_t st = (autoPos + kCap - autoFill) % kCap;
    for (size_t i = 0; i < autoFill; i++) a[i] = autoHist[(st + i) % kCap];
    const int fam = cfg.family;
    autoJob = std::async(std::launch::async, [v = std::move(a), fam]() { return kbdAutoAnalyse(v.data(), v.size(), fam, 3); });
}

void HfdigKbd::setConfig(const KbdConfig& c) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (c.family == p_->cfg.family && c.tones == p_->cfg.tones && c.bandwidth == p_->cfg.bandwidth && c.variant == p_->cfg.variant &&
        c.centreHz == p_->cfg.centreHz && c.autoCentre == p_->cfg.autoCentre && c.autoParams == p_->cfg.autoParams && !p_->cfgNew) return;
    if (c.family != p_->cfg.family || !c.autoParams) { p_->autoHave = false; p_->autoLocked = false; p_->autoFill = p_->autoPos = 0; p_->autoLastRun = p_->samples; p_->autoDiscard = p_->autoJob.valid(); }
    p_->cfg = c;
    p_->cfgNew = true;
}
KbdConfig HfdigKbd::config() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->cfg; }
void HfdigKbd::clearPictures() { std::lock_guard<std::mutex> lk(p_->mu); p_->t.picLive.reset(); p_->t.picHistory.clear(); p_->t.picSerial++; }
void HfdigKbd::clearText() { std::lock_guard<std::mutex> lk(p_->mu); p_->t.text.clear(); p_->t.chars = 0; p_->t.callsign.clear(); p_->lastCall.clear(); p_->hellCols.clear(); p_->t.hellTotal = 0; }
void HfdigKbd::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
std::unique_ptr<HfdigKbd> makeKbdDecoder() { return std::make_unique<HfdigKbd>(); }

// ------------------------------------------------------------------ transmitter
namespace {

class Tx : public HfdigTestAudio {
public:
    Tx(const KbdTxConfig& c) : c_(c) {
        KbdConfig k = c.cfg;
        if (k.variant < 0 || k.variant >= kbdVariantCount(k.family)) k.variant = kbdDefaultVariant(k.family);
        const double centre = k.centreHz + c.offsetHz;
        switch (k.family) {
        case KbdOlivia: case KbdContestia: mod_ = kbd::makeOliviaMod(k, centre); break;
        case KbdMfsk: mod_ = kbd::makeMfskMod(k, centre, c.picture, c.pictureAfterChars); break;
        case KbdThor: case KbdDominoex: mod_ = kbd::makeIfkMod(k, centre); break;
        default: mod_ = kbd::makeHellMod(k, centre); break;
        }
        src_.text = c.text;
        raw_.assign((size_t)(c.leadSec * kbd::kFs), 0.f);
        // normalise the amplitude: the modulators send about +-1 at the peak; the first symbols tell
    }
    void generate(float* out, size_t n) override {
        const double step = 1.0 + c_.clockPpm * 1e-6;
        for (size_t i = 0; i < n; i++) {
            while ((size_t)pos_ + 2 >= raw_.size()) { if (mod_) mod_->next(raw_, src_); else raw_.push_back(0); }
            const size_t k = (size_t)pos_;
            const double fr = pos_ - (double)k;
            out[i] = (float)(c_.amp * ((1 - fr) * raw_[k] + fr * raw_[k + 1]));
            pos_ += step;
            if (pos_ > 16384) { const size_t drop = (size_t)pos_ - 8; raw_.erase(raw_.begin(), raw_.begin() + drop); pos_ -= (double)drop; }
        }
    }
private:
    KbdTxConfig c_;
    std::unique_ptr<kbd::Modulator> mod_;
    kbd::TextSrc src_;
    std::vector<float> raw_;
    double pos_ = 0;
};

} // namespace

std::unique_ptr<HfdigTestAudio> makeKbdTx(const KbdTxConfig& c) { return std::make_unique<Tx>(c); }

std::unique_ptr<HfdigTestAudio> makeKbdTestAudio(const SynthConfig& cfg) {
    KbdTxConfig c;
    const int f = cfg.modeOpt[0] - 26;
    if (f < 0 || f >= kKbdFamilies) return nullptr;
    c.cfg.family = f;
    if (f == KbdOlivia || f == KbdContestia) {
        c.cfg.tones = cfg.modeOpt[1] >= 1 && cfg.modeOpt[1] <= 8 ? kKbdTonesList[cfg.modeOpt[1] - 1] : 32;
        c.cfg.bandwidth = cfg.modeOpt[2] >= 1 && cfg.modeOpt[2] <= 5 ? kKbdBwList[cfg.modeOpt[2] - 1] : 1000;
    } else {
        c.cfg.variant = cfg.modeOpt[1] >= 1 && cfg.modeOpt[1] <= kbdVariantCount(f) ? cfg.modeOpt[1] - 1 : kbdDefaultVariant(f);
    }
    c.offsetHz = cfg.modeVal[0];
    c.clockPpm = cfg.modeVal[1];
    return makeKbdTx(c);
}

} // namespace dect2
