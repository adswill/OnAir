// Iridium voice calls (iridium_voice.h). Clean-room: only the frame layout the public iridium-toolkit descriptions give; the vocoder is the
// user's plug-in (id "iridium", codec_registry.h).
#include "dect2/iridium/iridium_voice.h"
#include "dect2/audio/audioout.h"
#include "dect2/codec/codec_registry.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>

namespace dect2 {

namespace {
constexpr size_t kLcwBits = 46, kVoiceBits = 312;
constexpr double kCallGap = 1.0;        // seconds without a frame that end a call
constexpr double kSameFreqHz = 4000;    // a call's frequency drifts with Doppler; frames this close belong together
constexpr size_t kMaxCalls = 100, kMaxActive = 8;
constexpr size_t kMaxPcm = 8000 * 300;  // five minutes kept per call
constexpr int kMinPeak = 64;            // a call whose loudest sample is below this (about -54 dBFS) is silence: no WAV is written
constexpr double kCtxSec = 5, kCtxHz = 3000;
}

bool iridiumVoicePayload(const IridiumBurstBits& b, std::vector<uint8_t>& out) {
    out.clear();
    if (b.bits.size() < kLcwBits + kVoiceBits) return false;
    // the demodulator's bit order has the two bits of every symbol swapped (see IridiumBurstBits)
    auto bit = [&](size_t i) { return b.bits[i ^ 1] & 1; };
    for (size_t k = 0; k < kIridiumVoiceBytes; k++) {
        uint8_t v = 0;
        // as iridium-toolkit's bits_to_dfs.py writes the .dfs file the AMBE decoder reads: the first bit received is the LEAST significant
        for (int i = 0; i < 8; i++) v = uint8_t(v | (bit(kLcwBits + 8 * k + i) << i));
        out.push_back(v);
    }
    return true;
}

std::vector<uint8_t> iridiumBuildVoice(const std::vector<uint8_t>& payload, int lcw2, int lcw3) {
    std::vector<uint8_t> p(kVoiceBits, 0);
    for (size_t k = 0; k < kIridiumVoiceBytes && k < payload.size(); k++)
        for (int i = 0; i < 8; i++) p[8 * k + i] = (payload[k] >> i) & 1;
    return iridiumBuildLcwFrame(0, lcw2, lcw3, p);
}

struct IridiumVoice::Impl {
    struct Call {
        IridiumCall c;
        std::unique_ptr<codec::SpeechCodec> codec;
        double lastTry = -1e9;
        std::vector<int16_t> pcm;
        int rate = 8000;
        float upPrev = 0;
        bool saved = false;
        int peak = 0;                       // loudest sample of the call's sound, 0..32768
        bool mute = false, solo = false;
    };
    struct Ctx { double t, f; int sat, beam; };

    mutable std::mutex mu;
    std::deque<Call> calls;
    std::deque<Ctx> ctx;
    int nextId = 1;
    int listen = 0, autoId = -1;
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false}, silent{false};
    std::function<void(int, const float*, size_t)> tap;
    std::string recDir;
    std::unique_ptr<AudioOut> out;
    uint64_t toCodec = 0;
    std::vector<float> st;

    static const char* statusOf(const Call& k, bool codecThere) {
        if (k.c.decoded && k.peak < kMinPeak) return "silent audio";   // the plug-in answered, but only zeros: nothing is saved
        if (k.c.decoded) return "decoded";
        if (k.c.frames > 0 && k.c.silent == k.c.frames) return "silence";
        if (!k.c.readable) return "not readable";
        return codecThere ? "not readable" : "codec needed";   // with the plug-in loaded, every frame was refused
    }

    void save(Call& k) {
        if (k.saved) return;
        k.saved = true;
        if (recDir.empty() || k.pcm.empty()) return;
        if (k.peak < kMinPeak) { k.c.warning = "The codec plug-in returned only silence for this call (wrong plug-in, or the voice frames are not in the layout it expects). No WAV was written."; return; }
        char name[160];
        snprintf(name, sizeof name, "/iridium-call-%03d-%.3fMHz-%s.wav", k.c.id, k.c.freqHz / 1e6, k.c.downlink ? "down" : "up");
        const std::string path = recDir + name;
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) { k.c.warning = "The recording could not be written: " + path; return; }
        auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) fputc(int((v >> (8 * i)) & 255), f); };
        auto u16 = [&](uint16_t v) { fputc(v & 255, f); fputc(v >> 8, f); };
        const uint32_t bytes = uint32_t(k.pcm.size() * 2);
        fwrite("RIFF", 1, 4, f); u32(36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
        u32(16); u16(1); u16(1); u32(uint32_t(k.rate)); u32(uint32_t(k.rate) * 2); u16(2); u16(16);
        fwrite("data", 1, 4, f); u32(bytes);
        for (int16_t s : k.pcm) u16(uint16_t(s));
        const bool wrote = fflush(f) == 0 && !ferror(f);
        fclose(f);
        if (!wrote) { std::remove(path.c_str()); k.c.warning = "The recording could not be written (disk full?): " + path; return; }
        k.c.file = path;
        k.c.wallEnd = int64_t(std::time(nullptr));
    }

    void close(Call& k) {
        if (!k.c.active && k.saved) return;
        k.c.active = false;
        save(k);
        k.c.status = statusOf(k, codec::pluginLoaded("iridium"));
        k.codec.reset();
    }

    bool anySolo() const { for (const auto& k : calls) if (k.solo) return true; return false; }
    // is this call played: soloed calls alone, else the Listen choice; a muted call never
    bool isAudible(const Call& k, bool soloAny) {
        if (k.mute) return false;
        if (soloAny) return k.solo;
        return listen > 0 ? listen == k.c.id : listen == 0 ? (autoId < 0 ? (autoId = k.c.id, true) : autoId == k.c.id) : false;
    }

    void play(Call& k, const std::vector<int16_t>& pcm) {
        // linear interpolation to 48 kHz mono
        const double ratio = 48000.0 / std::max(1, k.rate);
        st.clear();
        const size_t n = size_t(std::lround(double(pcm.size()) * ratio));
        for (size_t i = 0; i < n; i++) {
            const double x = double(i) / ratio;
            const size_t a = std::min(pcm.size() - 1, size_t(x));
            const size_t b = std::min(pcm.size() - 1, a + 1);
            const double fr = x - double(a);
            st.push_back(float((pcm[a] * (1 - fr) + pcm[b] * fr) / 32768.0));
        }
        if (tap) tap(k.c.id, st.data(), st.size());
        if (silent.load()) return;
        if (!out) { out = std::make_unique<AudioOut>(); out->start(48000); out->setStartThreshold(48000 / 5); }
        out->setVolume(volume.load()); out->setMuted(muted.load());
        if (out->bufferedFrames() > 48000 * 3 / 2) out->flush();
        std::vector<float> stereo(st.size() * 2);
        for (size_t i = 0; i < st.size(); i++) stereo[2 * i] = stereo[2 * i + 1] = st[i];
        out->write(stereo.data(), int(st.size()));
    }

    void expireLocked(double t) {
        for (auto& k : calls) if (k.c.active && t - k.c.last > kCallGap) close(k);
        if (autoId >= 0) {
            bool alive = false;
            for (const auto& k : calls) if (k.c.id == autoId && k.c.active) alive = true;
            if (!alive) autoId = -1;
        }
    }
};

IridiumVoice::IridiumVoice() : p_(std::make_unique<Impl>()) {}
IridiumVoice::~IridiumVoice() { closeAll(); }

void IridiumVoice::feed(const IridiumBurstBits& b, const IridiumFrame& f) {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    s.expireLocked(b.timeSec);
    if (f.type != IridiumType::Voice) {
        if (f.ok && f.satId >= 0) {
            s.ctx.push_back({b.timeSec, b.freqHz, f.satId, f.beamId});
            while (s.ctx.size() > 64 || (!s.ctx.empty() && b.timeSec - s.ctx.front().t > kCtxSec)) s.ctx.pop_front();
        }
        return;
    }
    std::vector<uint8_t> pay;
    const bool have = iridiumVoicePayload(b, pay);
    Impl::Call* k = nullptr;
    for (auto& c : s.calls)
        if (c.c.active && c.c.downlink == b.downlink && std::fabs(c.c.freqHz - b.freqHz) < kSameFreqHz) { k = &c; break; }
    if (!k) {
        size_t active = 0;
        for (auto& c : s.calls) active += c.c.active;
        if (active >= kMaxActive) return;
        Impl::Call n;
        n.c.id = s.nextId++;
        n.c.start = b.timeSec;
        n.c.downlink = b.downlink;
        n.c.active = true;
        for (auto it = s.ctx.rbegin(); it != s.ctx.rend(); ++it)
            if (b.timeSec - it->t < kCtxSec && std::fabs(it->f - b.freqHz) < kCtxHz) { n.c.sat = it->sat; n.c.beam = it->beam; break; }
        s.calls.push_back(std::move(n));
        while (s.calls.size() > kMaxCalls) {
            size_t drop = 0;
            while (drop < s.calls.size() && s.calls[drop].c.active) drop++;
            if (drop >= s.calls.size()) break;
            s.calls.erase(s.calls.begin() + long(drop));
        }
        k = &s.calls.back();
    }
    k->c.last = b.timeSec;
    k->c.freqHz = b.freqHz;
    k->c.frames++;
    const bool uniform = have && std::all_of(pay.begin(), pay.end(), [&](uint8_t v) { return v == pay[0]; });
    const bool silentFrame = f.lcw2 == 15 && f.lcw3 == 0x100000;   // iridium-toolkit: LCW(0,001111,100000000000000000000) is silence
    if (silentFrame) k->c.silent++;
    const bool readable = f.ok && have && !uniform && !silentFrame;   // the link control word checked out and the field is not a fill pattern
    if (readable) k->c.readable++;
    if (readable && !k->codec && b.timeSec - k->lastTry >= 1.0) {
        k->lastTry = b.timeSec;
        k->codec = codec::pluginVocoder("iridium");
        if (k->codec) k->rate = std::max(1000, k->codec->sampleRate());
    }
    if (readable && k->codec) {
        std::vector<int16_t> pcm = k->codec->decode(pay);
        s.toCodec++;
        if (!pcm.empty()) {
            k->c.decoded++;
            k->c.audioSec += double(pcm.size()) / k->rate;
            if (k->pcm.size() < kMaxPcm) k->pcm.insert(k->pcm.end(), pcm.begin(), pcm.end());
            int pk = 0;
            for (int16_t x : pcm) pk = std::max(pk, std::abs(int(x)));
            k->peak = std::max(k->peak, pk);
            k->c.level = float(pk) / 32768.f;
            k->c.audible = s.isAudible(*k, s.anySolo());
            if (k->c.audible) s.play(*k, pcm);
        }
    }
    k->c.mute = k->mute; k->c.solo = k->solo;
    k->c.status = Impl::statusOf(*k, codec::pluginLoaded("iridium"));
    if (k->c.status == "codec needed")
        k->c.warning = "No Iridium voice codec is installed, so no sound and no WAV file. Install your own plug-in (docs/CODECS.md).";
    else if (k->c.status == "silent audio" && k->c.warning.empty() && k->c.decoded >= 25)
        k->c.warning = "The codec plug-in returns only silence so far (wrong plug-in, or the voice frames are not in the layout it expects).";
    else if (k->c.status == "decoded") k->c.warning.clear();
}

void IridiumVoice::expire(double t) { std::lock_guard<std::mutex> lk(p_->mu); p_->expireLocked(t); }

void IridiumVoice::closeAll() {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (auto& k : p_->calls) p_->close(k);
    p_->autoId = -1;
}

void IridiumVoice::clear() {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (auto& k : p_->calls) p_->close(k);
    p_->calls.clear();
    p_->ctx.clear();
    p_->autoId = -1;
}

std::vector<IridiumCall> IridiumVoice::calls() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<IridiumCall> v;
    for (const auto& k : p_->calls) v.push_back(k.c);
    return v;
}

uint64_t IridiumVoice::framesToCodec() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->toCodec; }
void IridiumVoice::setListenCall(int id) { std::lock_guard<std::mutex> lk(p_->mu); p_->listen = id; if (id == 0) p_->autoId = -1; }
void IridiumVoice::setMute(int id, bool on) { std::lock_guard<std::mutex> lk(p_->mu); for (auto& k : p_->calls) if (k.c.id == id) k.mute = k.c.mute = on; }
void IridiumVoice::setSolo(int id, bool on) { std::lock_guard<std::mutex> lk(p_->mu); for (auto& k : p_->calls) if (k.c.id == id) k.solo = k.c.solo = on; }
void IridiumVoice::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void IridiumVoice::setMuted(bool m) { p_->muted = m; }
void IridiumVoice::setSilent(bool s) { p_->silent = s; }
void IridiumVoice::setAudioTap(std::function<void(int, const float*, size_t)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->tap = std::move(cb); }
void IridiumVoice::setRecordFolder(const std::string& dir) { std::lock_guard<std::mutex> lk(p_->mu); p_->recDir = dir; }
bool IridiumVoice::codecReady() const { return codec::pluginLoaded("iridium"); }

} // namespace dect2
