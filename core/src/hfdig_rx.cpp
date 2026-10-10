// HF digital receiver (see hfdig_rx.h): the channel to 24 kHz complex, the upper sideband to 8 kHz audio, the three decoders on that
// audio, and FreeDV's speech to the sound card. The decoders themselves are in hfdig_rtty.cpp, hfdig_sstv.cpp and hfdig_freedv.cpp.
#include "dect2/hfdig_rx.h"
#include "dect2/hfdig_tel.h"
#include "dect2/hfdig_rtty.h"
#include "dect2/hfdig_sstv.h"
#include "dect2/hfdig_freedv.h"
#include "dect2/hfdig_ftx.h"
#include "dect2/marine_dsp.h"
#include "dect2/audioout.h"
#include "hfdig_usb.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

namespace {

// Upper sideband audio: 24 kHz complex (the dial frequency at 0 Hz) -> real audio at 8 kHz, 200 .. 3800 Hz, with a slow AGC
class UsbAudio {
public:
    static constexpr int kTaps = 383;          // about 270 Hz of transition at 24 kHz: the lower sideband is gone 200 Hz below the dial
    static constexpr int kDecim = 3;           // 24 kHz -> 8 kHz
    UsbAudio() {
        const auto h = hfdig::usbTaps(kTaps, marine::kBaseRate);
        taps_.resize(kTaps);
        for (int j = 0; j < kTaps; j++) taps_[(size_t)j] = h[(size_t)(kTaps - 1 - j)];   // the history is kept oldest first
        hist_.assign(2 * kTaps, cf32(0.f, 0.f));
    }
    void reset() { std::fill(hist_.begin(), hist_.end(), cf32(0.f, 0.f)); pos_ = 0; phase_ = 0; env_ = 0; }
    // appends to out; adds |band|^2 of every output sample to power (before the AGC)
    void process(const cf32* x, size_t n, std::vector<float>& out, double& power) {
        for (size_t i = 0; i < n; i++) {
            hist_[(size_t)pos_] = x[i]; hist_[(size_t)(pos_ + kTaps)] = x[i];   // twice, so the window never wraps
            if (++pos_ >= kTaps) pos_ = 0;
            if (++phase_ < kDecim) continue;
            phase_ = 0;
            const cf32* w = &hist_[(size_t)pos_];   // the last kTaps samples, oldest first
            float ar = 0, ai = 0;
            for (int k = 0; k < kTaps; k++) {
                const cf32 v = w[k], t = taps_[(size_t)k];
                ar += t.real() * v.real() - t.imag() * v.imag();
                ai += t.real() * v.imag() + t.imag() * v.real();
            }
            const float mag2 = ar * ar + ai * ai;
            power += mag2;
            const float mag = std::sqrt(mag2);
            env_ += (mag > env_ ? 0.01f : 0.0001f) * (mag - env_);   // fast attack, slow release
            const float g = 0.5f / (env_ + 1e-9f);
            out.push_back(std::max(-1.f, std::min(1.f, ar * g)));
        }
    }
private:
    std::vector<cf32> taps_, hist_;
    int pos_ = 0, phase_ = 0;
    float env_ = 0;
};

// FreeDV's speech: 8 kHz mono -> 48 kHz stereo for the sound card (polyphase interpolation by 6, pass band up to 3.6 kHz)
class Upsample6 {
public:
    static constexpr int kUp = 6, kPerPhase = 24;
    Upsample6() {
        const auto lp = hfdig::lowpass(kUp * kPerPhase, 3600.0 / (kUp * kHfdigAudioRate), 7.0);
        for (int p = 0; p < kUp; p++)
            for (int k = 0; k < kPerPhase; k++) taps_[p][k] = (float)(kUp * lp[(size_t)(p + kUp * k)]);
        reset();
    }
    void reset() { for (auto& v : hist_) v = 0; pos_ = 0; }
    void process(const float* x, size_t n, std::vector<float>& stereo) {
        for (size_t i = 0; i < n; i++) {
            hist_[pos_] = x[i]; hist_[pos_ + kPerPhase] = x[i];
            if (++pos_ >= kPerPhase) pos_ = 0;
            const float* w = &hist_[pos_];   // oldest first: w[kPerPhase - 1] is x[i]
            for (int p = 0; p < kUp; p++) {
                float s = 0;
                for (int k = 0; k < kPerPhase; k++) s += taps_[p][k] * w[kPerPhase - 1 - k];
                stereo.push_back(s); stereo.push_back(s);
            }
        }
    }
private:
    float taps_[kUp][kPerPhase] = {};
    float hist_[2 * kPerPhase] = {};
    int pos_ = 0;
};

} // namespace

struct HfdigReceiver::Impl {
    std::mutex mu;                         // guards the members up to pub
    std::function<void(const std::string&)> log;
    std::function<void(const float*, size_t)> audioTapSet, speechTapSet;
    double rate = 0;                       // from configure()
    double offsetHz = 0;                   // setSignalOffset()
    HfdigTelemetry pub;                    // the published report
    std::atomic<bool> resetReq{true}, offsetReq{false}, tapsReq{false};
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false}, silent{false};
    // receiver thread
    HfdigTelemetry tel;
    double curRate = 0;
    bool running = false;                  // the rate is high enough for the front end
    int64_t nIn = 0;
    double nextReport = 0;
    double power = 0, audioPower = 0;      // sum of |x|^2 of the input and of the sideband since the last report
    int64_t nPower = 0, nAudio = 0;
    marine::Front front;
    UsbAudio usb;
    std::vector<cf32> base;
    std::vector<float> audio;
    std::unique_ptr<HfdigRtty> rtty = makeRttyDecoder();
    std::unique_ptr<HfdigSstv> sstv = makeSstvDecoder();
    std::unique_ptr<HfdigFreedv> freedv = makeFreedvDecoder();
    std::unique_ptr<HfdigFtx> ftx = makeFtxDecoder();
    HfdigDecoder* decoders[4] = {rtty.get(), sstv.get(), freedv.get(), ftx.get()};
    std::function<void(const float*, size_t)> audioTap, speechTap;
    Upsample6 up;
    std::vector<float> pcm;
    std::unique_ptr<AudioOut> audioOut;

    Impl() { freedv->setSpeechOut([this](const float* x, size_t n) { playSpeech(x, n); }); }

    void resetState() {
        std::function<void(const std::string&)> cb;
        double off;
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; off = offsetHz; cb = log; }
        const uint64_t s = tel.seq;        // the report number goes on
        tel = HfdigTelemetry();
        tel.seq = s;
        tel.inputRate = curRate;
        nIn = 0; nextReport = 0; power = audioPower = 0; nPower = nAudio = 0;
        running = curRate >= hfdigTuning().minSampleRate - 1;
        if (running) { front.configure(curRate); front.setOffsetHz(off); }
        usb.reset(); up.reset();
        base.clear(); audio.clear(); pcm.clear();
        for (HfdigDecoder* d : decoders) d->reset();
        if (audioOut) audioOut->flush();
        if (cb && curRate > 0) cb("HF digital: listening to the sideband audio for RTTY, SSTV, FreeDV, FT8, FT4, FT2 and WSPR");
    }

    void playSpeech(const float* x, size_t n) {
        if (speechTap) speechTap(x, n);
        if (silent.load() || n == 0) return;
        pcm.clear();
        up.process(x, n, pcm);
        if (!audioOut) {
            audioOut = std::make_unique<AudioOut>();
            audioOut->start(48000);
            audioOut->setStartThreshold(48000 / 4);
        }
        audioOut->setVolume(volume.load()); audioOut->setMuted(muted.load());
        if (audioOut->bufferedFrames() > 48000 * 3 / 2) audioOut->flush();   // the radio's clock runs ahead of the sound card: catch up
        audioOut->write(pcm.data(), (int)(pcm.size() / 2));
    }

    void report() {
        tel.seq++;
        tel.timeSec = curRate > 0 ? (double)nIn / curRate : 0;
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        tel.audioDb = nAudio ? (float)(10 * std::log10(audioPower / (double)nAudio + 1e-20)) : -200.f;
        power = audioPower = 0; nPower = nAudio = 0;
        rtty->telemetry(tel.rtty);
        sstv->telemetry(tel.sstv);
        freedv->telemetry(tel.freedv);
        ftx->telemetry(tel.ftx);
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }
};

HfdigReceiver::HfdigReceiver() : p_(std::make_unique<Impl>()) {}
HfdigReceiver::~HfdigReceiver() = default;

// configure() and reset() may come from another thread than feed(): they only leave a request that feed() carries out
void HfdigReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = inputRateHz;
    p_->resetReq = true;
}
void HfdigReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; p_->offsetReq = true; }
bool HfdigReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rate >= hfdigTuning().minSampleRate - 1;
}
void HfdigReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetReq = true;
    const uint64_t s = p_->pub.seq;
    p_->pub = HfdigTelemetry();
    p_->pub.seq = s;
}

void HfdigReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.tapsReq.exchange(false)) { std::lock_guard<std::mutex> lk(m.mu); m.audioTap = m.audioTapSet; m.speechTap = m.speechTapSet; }
    if (m.resetReq.exchange(false)) { m.offsetReq = false; m.resetState(); }
    if (m.offsetReq.exchange(false) && m.running) { std::lock_guard<std::mutex> lk(m.mu); m.front.setOffsetHz(m.offsetHz); }
    if (m.curRate <= 0) return;
    for (size_t i = 0; i < n; i++) m.power += (double)std::norm(x[i]);
    m.nPower += (int64_t)n;
    m.nIn += (int64_t)n;
    if (m.running) {
        m.base.clear(); m.audio.clear();
        m.front.process(x, n, m.base);
        m.usb.process(m.base.data(), m.base.size(), m.audio, m.audioPower);
        m.nAudio += (int64_t)m.audio.size();
        if (!m.audio.empty()) {
            if (m.audioTap) m.audioTap(m.audio.data(), m.audio.size());
            for (HfdigDecoder* d : m.decoders) d->feedAudio(m.audio.data(), m.audio.size());
        }
    }
    // about four reports a second of signal
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.curRate;
        m.report();
    }
}

bool HfdigReceiver::telemetry(HfdigTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void HfdigReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void HfdigReceiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void HfdigReceiver::setMuted(bool m) { p_->muted = m; }
void HfdigReceiver::setSilent(bool s) { p_->silent = s; }
void HfdigReceiver::setAudioTap(std::function<void(const float* x, size_t n)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->audioTapSet = std::move(cb); p_->tapsReq = true; }
void HfdigReceiver::setSpeechTap(std::function<void(const float* x, size_t n)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->speechTapSet = std::move(cb); p_->tapsReq = true; }

HfdigRtty& HfdigReceiver::rtty() { return *p_->rtty; }
HfdigFtx& HfdigReceiver::ftx() { return *p_->ftx; }

ModeTuning hfdigTuning() {
    ModeTuning t;
    t.stdMode = 27; t.id = "hfdig"; t.name = "HF digital";
    t.minMhz = 1.0; t.maxMhz = 30; t.defMhz = 14.230;   // 14.230 MHz: the 20 m SSTV calling frequency (upper sideband)
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.003;
    t.minSampleRate = 1000000;
    t.tuneOffsetHz = 20000;                              // as marine: the radio sits 20 kHz above the dial frequency, away from its DC spike
    return t;
}

} // namespace dect2
