#include "dect2/fm/fm_neighbour.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

using dect2::math::kPi;

namespace {
constexpr int kScanN = 8192;
constexpr double kPass = 115e3, kStop = 175e3;   // the neighbour's own filter: enough of it for its FM to be regenerated, little of the wanted station

std::vector<float> lowpass(double pass, double stop, double fs) {
    const double fc = 0.5 * (pass + stop) / fs, df = (stop - pass) / fs;
    const double atten = 50, beta = 0.1102 * (atten - 8.7);
    int n = (int)std::ceil((atten - 7.95) / (14.36 * df)) + 1;
    n |= 1;
    auto i0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 40; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
    std::vector<float> h((size_t)n);
    double sum = 0;
    const int m = n / 2;
    for (int i = 0; i < n; i++) {
        const double x = i - m, r = x / (m + 0.5);
        const double w = i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0(beta);
        h[(size_t)i] = (float)((x == 0 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x)) * w);
        sum += h[(size_t)i];
    }
    for (auto& v : h) v = (float)(v / sum);
    return h;
}
}

void FmNeighbourCancel::configureIf(double rate) {
    if (rate == rate_) return;
    rate_ = rate;
    taps_ = lowpass(kPass, kStop, rate);
    nt_ = (int)taps_.size();
    delay_ = nt_ / 2;
    hist_.assign((size_t)(2 * nt_ + 2), cf32(0, 0));
    firIn_.assign((size_t)(2 * nt_), cf32(0, 0));
    fft_ = std::make_unique<Fft>(kScanN);
    sp_.resize(kScanN); pw_.assign(kScanN, 0.f); scan_.assign(kScanN, cf32(0, 0));
    reset();
}

void FmNeighbourCancel::reset() {
    pos_ = 0; offset_ = 0; rot_ = cf32(1, 0); prevN_ = cf32(1, 0); g_ = cf32(0, 0); ph_ = 0; pAvg_ = 1e-9f; levelDb_ = 0; fill_ = 0; frames_ = 0; activeHold_ = 0;
    std::fill(pw_.begin(), pw_.end(), 0.f);
    std::fill(firIn_.begin(), firIn_.end(), cf32(0, 0));
    std::fill(hist_.begin(), hist_.end(), cf32(0, 0));
}

void FmNeighbourCancel::detect(const cf32* x, size_t n) {
    for (size_t i = 0; i < n; i++) {
        scan_[fill_++] = x[i];
        if (fill_ < kScanN) continue;
        fill_ = 0;
        for (int k = 0; k < kScanN; k++) {
            const float w = 0.5f - 0.5f * (float)std::cos(2 * kPi * k / kScanN);
            const cf32 v = scan_[(size_t)k];
            sp_[(size_t)k] = std::isfinite(v.real() + v.imag()) ? v * w : cf32(0, 0);
        }
        fft_->forward(sp_.data());
        for (int k = 0; k < kScanN; k++) pw_[(size_t)((k + kScanN / 2) % kScanN)] += std::norm(sp_[(size_t)k]);
        if (++frames_ < 20) continue;
        const double bin = rate_ / kScanN;
        auto band = [&](double f) {   // mean power of +-75 kHz around f, outside the DC bins
            double s = 0; int c = 0;
            for (int b = (int)((f - 75e3) / bin); b <= (int)((f + 75e3) / bin); b++) {
                if (std::abs(b) < 3) continue;
                const int q = b + kScanN / 2;
                if (q >= 0 && q < kScanN) { s += pw_[(size_t)q]; c++; }
            }
            return c ? s / c : 0.0;
        };
        const double me = band(0);
        double best = 0, bestOff = 0;
        for (double f : {-200e3, 200e3}) {
            if (std::fabs(f) + kStop > 0.47 * rate_) continue;
            const double p = band(f);
            if (p > best) { best = p; bestOff = f; }
        }
        const float dB = (float)(10 * std::log10(std::max(best, 1e-20) / std::max(me, 1e-20)));
        std::fill(pw_.begin(), pw_.end(), 0.f);
        frames_ = 0;
        if (dB >= 10) {
            levelDb_ = dB; activeHold_ = 3;
            if (bestOff != offset_) {
                offset_ = bestOff; g_ = cf32(0, 0); rot_ = cf32(1, 0);
                const double w = -2 * kPi * offset_ / rate_;
                rotStep_ = cf32((float)std::cos(w), (float)std::sin(w));
                std::fill(firIn_.begin(), firIn_.end(), cf32(0, 0));
                prevN_ = cf32(1, 0); ph_ = 0;
                delayRot_ = cf32((float)std::cos(-w * delay_), (float)std::sin(-w * delay_));
            }
        } else if (activeHold_ > 0 && --activeHold_ == 0) { offset_ = 0; levelDb_ = 0; }
    }
}

void FmNeighbourCancel::process(const cf32* x, size_t n, cf32* out) {
    detect(x, n);
    if (offset_ == 0) { std::copy(x, x + n, out); return; }
    for (size_t i = 0; i < n; i++) {
        const cf32 in = std::isfinite(x[i].real() + x[i].imag()) ? x[i] : cf32(0, 0);
        const size_t hsz = hist_.size();
        hist_[pos_ % hsz] = in;
        // the neighbour at DC, then its filter
        const cf32 mixed = in * rot_;
        firIn_[pos_ % (size_t)nt_] = mixed; firIn_[(pos_ % (size_t)nt_) + (size_t)nt_] = mixed;
        const cf32* h = &firIn_[(pos_ % (size_t)nt_) + 1];
        float re = 0, im = 0;
        for (int k = 0; k < nt_; k++) { re += taps_[(size_t)k] * h[k].real(); im += taps_[(size_t)k] * h[k].imag(); }
        const cf32 y(re, im);
        // its own limiter: the neighbour's waveform with constant amplitude
        const float a = std::abs(y);
        const cf32 lim = a > 1e-12f ? y / a : cf32(1, 0);
        // the amplitude and phase of what is really there: a slow complex gain against the regenerated wave
        const size_t dpos = (pos_ + hsz - (size_t)delay_) % hsz;
        const cf32 dIn = hist_[dpos];
        const cf32 backRot = std::conj(rot_ * delayRot_);   // the oscillator as it was delay_ samples ago, back up to the neighbour's frequency
        const cf32 s = lim * backRot;
        const cf32 e = dIn - g_ * s;
        g_ += e * std::conj(s) * 0.015f;
        out[i] = dIn - g_ * s;
        rot_ *= rotStep_;
        if ((pos_ & 4095) == 0) rot_ /= std::abs(rot_);
        pos_++;
    }
}

} // namespace dect2
