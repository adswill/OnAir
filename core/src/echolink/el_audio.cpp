// EchoLink audio helpers: see el_audio.h.
#include "dect2/echolink/el_audio.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace echolink {

namespace {
constexpr int kTaps = 121;
// Blackman-windowed sinc low-pass for 48 kHz: pass 3.4 kHz, about -70 dB from 4.6 kHz on (8 kHz output aliases above 4 kHz)
std::vector<float> lowpass48k() {
    std::vector<float> h(kTaps);
    const double fc = 4000.0 / 48000.0;
    double sum = 0;
    for (int i = 0; i < kTaps; i++) {
        const double m = i - (kTaps - 1) / 2.0;
        const double s = m == 0 ? 2 * fc : std::sin(2 * M_PI * fc * m) / (M_PI * m);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (kTaps - 1)) + 0.08 * std::cos(4 * M_PI * i / (kTaps - 1));
        h[i] = (float)(s * w);
        sum += h[i];
    }
    for (float& v : h) v = (float)(v / sum);
    return h;
}
inline int16_t toS16(float v) {
    const float s = std::round(v * 32767.0f);
    return (int16_t)std::max(-32768.0f, std::min(32767.0f, s));
}
} // namespace

Down6::Down6() : h_(lowpass48k()), hist_(2 * kTaps, 0.f) {}
void Down6::reset() { std::fill(hist_.begin(), hist_.end(), 0.f); pos_ = 0; phase_ = 0; }
void Down6::process(const float* in, int n, std::vector<int16_t>& out) {
    for (int i = 0; i < n; i++) {
        hist_[pos_] = hist_[pos_ + kTaps] = in[i];
        pos_ = (pos_ + 1) % kTaps;
        if (++phase_ == 6) {
            phase_ = 0;
            float acc = 0;
            const float* x = &hist_[pos_];   // oldest first
            for (int k = 0; k < kTaps; k++) acc += h_[k] * x[k];
            out.push_back(toS16(acc));
        }
    }
}

Up6::Up6() : h_(lowpass48k()), hist_(2 * kTaps, 0.f) {}
void Up6::reset() { std::fill(hist_.begin(), hist_.end(), 0.f); pos_ = 0; }
void Up6::process(const int16_t* in, int n, std::vector<float>& out) {
    for (int i = 0; i < n; i++) {
        for (int ph = 0; ph < 6; ph++) {
            const float v = ph == 0 ? 6.0f * in[i] / 32768.0f : 0.f;
            hist_[pos_] = hist_[pos_ + kTaps] = v;
            pos_ = (pos_ + 1) % kTaps;
            float acc = 0;
            const float* x = &hist_[pos_];
            for (int k = 0; k < kTaps; k++) acc += h_[k] * x[k];
            out.push_back(acc);
        }
    }
}

bool Vox::process(const int16_t* pcm, int n) {
    for (int i = 0; i < n; i++) {
        acc_ += (double)pcm[i] * pcm[i];
        if (++accN_ == 160) {   // 20 ms
            const double rms = std::sqrt(acc_ / accN_) / 32768.0;
            levelDb_ = rms > 0 ? 20 * std::log10(rms) : -120;
            acc_ = 0; accN_ = 0;
            if (levelDb_ >= cfg_.thresholdDbfs) { open_ = true; quietMs_ = 0; }
            else if (open_ && (quietMs_ += 20) >= cfg_.holdMs) open_ = false;
        }
    }
    return open_;
}

BridgeOut RfBridge::step(int dtMs, bool rfSquelchOpen, bool echoLinkRx) {
    BridgeOut o;
    if (!active()) { reset(); return o; }
    if (lockout_) {   // after a time-out: wait until EchoLink is quiet before the radio may transmit again
        if (!echoLinkRx) lockout_ = false;
        else { o.timedOut = true; return o; }
    }
    switch (dir_) {
    case None:
        if (echoLinkRx) { dir_ = NetToRf; txMs_ = 0; tailLeft_ = cfg_.tailMs; }
        else if (rfSquelchOpen) dir_ = RfToNet;
        break;
    case RfToNet:
        if (!rfSquelchOpen) dir_ = None;
        break;
    case NetToRf:
        if (echoLinkRx) tailLeft_ = cfg_.tailMs;
        else if ((tailLeft_ -= dtMs) <= 0) dir_ = None;
        break;
    }
    if (dir_ == NetToRf) {
        txMs_ += dtMs;
        if (cfg_.maxTxSec > 0 && txMs_ >= (long)cfg_.maxTxSec * 1000) {
            dir_ = None; lockout_ = true; o.timedOut = true;
            return o;
        }
        o.keyRadio = true;
    }
    o.forwardRfToEchoLink = dir_ == RfToNet;
    return o;
}

} // namespace echolink
} // namespace dect2
