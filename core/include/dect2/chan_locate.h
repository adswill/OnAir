// Where a flat, wideband channel (DVB-T, DTMB) sits inside the sample band, from the averaged power spectrum. For recordings made beside the
// channel (gqrx, SDR#) and radios tuned far off: the box of the channel's width with the most power, accepted only when both of its edges stand
// clearly above what lies beside them. Plus the mixer that moves the channel to the centre. Header only.
#pragma once
#include "fftutil.h"
#include <cmath>
#include <complex>
#include <vector>

namespace dect2 {

class ChannelLocator {
public:
    // rate: input sample rate; width: the channel's occupied (flat) width, Hz
    void configure(double rate, double width) { rate_ = rate; width_ = width; reset(); }
    void reset() { pow_.assign(kF, 0.0); blocks_ = 0; skip_ = 0; have_.clear(); }
    // Feed input; returns true when an estimate is ready (about every 50 ms of input). found: a clear channel was seen, at hz from the centre.
    bool feed(const std::complex<float>* x, size_t n, double& hz, bool& found) {
        const size_t stride = (size_t)std::max<double>(kF, rate_ * 0.0015);   // one FFT every 1.5 ms: a few percent of the cost of the receiver
        bool ready = false;
        for (size_t i = 0; i < n;) {
            if (skip_ > 0) { const size_t k = std::min(skip_, n - i); skip_ -= k; i += k; continue; }
            const size_t k = std::min((size_t)kF - have_.size(), n - i);
            have_.insert(have_.end(), x + i, x + i + k);
            i += k;
            if (have_.size() < (size_t)kF) break;
            block();
            have_.clear();
            skip_ = stride - kF;
            if (blocks_ >= kBlocks) { ready = true; estimate(hz, found); pow_.assign(kF, 0.0); blocks_ = 0; }
        }
        return ready;
    }

private:
    static constexpr int kF = 1024, kBlocks = 32;
    double rate_ = 0, width_ = 0;
    std::vector<double> pow_;
    std::vector<std::complex<float>> have_;
    int blocks_ = 0;
    size_t skip_ = 0;

    void block() {
        Fft f(kF);
        std::vector<std::complex<float>> b(have_);
        for (int i = 0; i < kF; i++) {
            if (!std::isfinite(b[(size_t)i].real()) || !std::isfinite(b[(size_t)i].imag())) b[(size_t)i] = 0;
            b[(size_t)i] *= (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / kF));
        }
        f.forward(b.data());
        for (int i = 0; i < kF; i++) pow_[(size_t)((i + kF / 2) % kF)] += std::norm(b[(size_t)i]);   // bin kF / 2 is 0 Hz
        blocks_++;
    }
    void estimate(double& hz, bool& found) {
        found = false; hz = 0;
        const int W = (int)std::lround(width_ / rate_ * kF);
        if (W < 16 || W > kF - 8) return;            // no room for the channel to be anywhere but the centre
        std::vector<double> cum(kF + 1, 0.0);
        for (int i = 0; i < kF; i++) cum[(size_t)i + 1] = cum[(size_t)i] + pow_[(size_t)i];
        int best = -1; double bs = -1;
        for (int a = 0; a + W <= kF; a++) { const double v = cum[(size_t)(a + W)] - cum[(size_t)a]; if (v > bs) { bs = v; best = a; } }
        if (best < 0 || bs <= 0) return;
        const double inside = bs / W;
        const int e = std::max(2, W / 25);
        auto edgeOk = [&](int a0, int a1) {
            a0 = std::max(0, a0); a1 = std::min(kF, a1);
            if (a1 - a0 < 2) return true;            // the band beside this edge is outside the input band
            return (cum[(size_t)a1] - cum[(size_t)a0]) / (a1 - a0) < 0.25 * inside;
        };
        if (!edgeOk(best - e - 2, best - 2) || !edgeOk(best + W + 2, best + W + e + 2)) return;
        hz = ((double)best + W / 2.0 - kF / 2.0) * rate_ / kF;
        found = true;
    }
};

// Multiplies the input by exp(-j 2 pi hz t): a channel at +hz moves to the centre
struct ChannelMixer {
    double hz = 0;
    std::complex<double> rot{1, 0}, step{1, 0};
    unsigned long n = 0;
    void set(double h, double rate) { hz = h; rot = 1; n = 0; step = std::polar(1.0, -2 * M_PI * h / rate); }
    void apply(std::complex<float>* x, size_t cnt) {
        if (hz == 0) return;
        for (size_t i = 0; i < cnt; i++) {
            const std::complex<double> v = std::complex<double>(x[i].real(), x[i].imag()) * rot;
            x[i] = std::complex<float>((float)v.real(), (float)v.imag());
            rot *= step;
            if ((++n & 1023) == 0) rot /= std::abs(rot);
        }
    }
};

} // namespace dect2
