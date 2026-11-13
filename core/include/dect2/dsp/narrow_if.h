// A narrow channel filter with a carrier follow-up in front of an FM discriminator (4.0 weak-signal path of the FM-based narrowband data modes).
// An FM discriminator sits at its threshold when the IF noise bandwidth is several times the signal's: the filter cuts it to the signal's own
// width, and the follow-up (the 50 ms mean of the discriminator is the carrier error) keeps a radio that is some kHz off in the middle of it.
// Usage per sample at fs: y = f.process(z); the discriminator runs on y; f.feedback(hz) with the discriminator's output in Hz.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include "dect2/common/mathconst.h"

namespace dect2 {

class NarrowIf {
public:
    // fcHz: -3 dB half width of the two section Butterworth low-pass; afcMaxHz: how far the follow-up goes
    void configure(double fs, double fcHz, double afcMaxHz = 4000.0) {
        fs_ = fs; afcMax_ = afcMaxHz;
        const double K = std::tan(math::kPi * fcHz / fs), q = std::sqrt(0.5), n = 1.0 / (1.0 + K / q + K * K);
        b0_ = (float)(K * K * n); b1_ = 2 * b0_; b2_ = b0_;
        a1_ = (float)(2 * (K * K - 1) * n); a2_ = (float)((1.0 - K / q + K * K) * n);
        meanA_ = 1.0 / (0.05 * fs);        // 50 ms
        loopG_ = 1.0 / (0.5 * fs);         // 0.5 s integrator
        reset();
    }
    void reset() {
        for (auto& a : zi_) a[0] = a[1] = 0;
        for (auto& a : zq_) a[0] = a[1] = 0;
        hz_ = ph_ = mean_ = 0;
    }
    bool on() const { return fs_ > 0; }
    std::complex<float> process(std::complex<float> x) {
        ph_ += 2 * math::kPi * hz_ / fs_;
        if (ph_ > math::kPi) ph_ -= 2 * math::kPi; else if (ph_ < -math::kPi) ph_ += 2 * math::kPi;
        x *= std::complex<float>((float)std::cos(ph_), (float)-std::sin(ph_));
        float xr = x.real(), xi = x.imag();
        for (int k = 0; k < 2; k++) {
            const float yr = b0_ * xr + zi_[k][0]; zi_[k][0] = b1_ * xr - a1_ * yr + zi_[k][1]; zi_[k][1] = b2_ * xr - a2_ * yr; xr = yr;
            const float yi = b0_ * xi + zq_[k][0]; zq_[k][0] = b1_ * xi - a1_ * yi + zq_[k][1]; zq_[k][1] = b2_ * xi - a2_ * yi; xi = yi;
        }
        return std::complex<float>(xr, xi);
    }
    void feedback(double discHz) {
        mean_ += (discHz - mean_) * meanA_;
        hz_ = std::max(-afcMax_, std::min(afcMax_, hz_ + mean_ * loopG_));
    }
    double offsetHz() const { return hz_; }
private:
    double fs_ = 0, afcMax_ = 4000, meanA_ = 0, loopG_ = 0;
    float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    float zi_[2][2] = {{0, 0}, {0, 0}}, zq_[2][2] = {{0, 0}, {0, 0}};
    double hz_ = 0, ph_ = 0, mean_ = 0;
};

}  // namespace dect2
