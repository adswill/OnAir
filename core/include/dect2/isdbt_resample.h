// Rational polyphase resampler for the ISDB-T test signal: the IFFT rate (512/63 MHz) to the radio rate (a small rational ratio, 315/256 for 10 Msps).
// The ISDB-T signal fills 5.6 MHz of the 8.13 MHz input band, so a short filter (24 taps per phase) is enough; the exact phases come from
// integer arithmetic, no per-sample position tracking.
#pragma once
#include "dect2/gen_util.h"
#include "ring.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace dect2 {
namespace isdbt {

class PolyResampler {
public:
    // false if out/in is not a ratio of small integers
    bool configure(double inRate, double outRate) {
        const double ratio = outRate / inRate;
        L_ = 0;
        for (int q = 1; q <= 1024 && !L_; q++) {
            const double p = ratio * q;
            if (std::fabs(p - std::round(p)) < 1e-9 * p && std::round(p) >= 1) { L_ = (int)std::round(p); M_ = q; }
        }
        if (!L_ || L_ > 4096) { L_ = 0; return false; }
        const int g = std::gcd(L_, M_);
        L_ /= g; M_ /= g;
        if (ratio > 4 || ratio < 0.95) { L_ = 0; return false; }   // other ratios: the caller uses the general resampler
        // windowed sinc (Kaiser, about 80 dB), cut-off in the middle of the gap between the signal edge (2.9 MHz) and the first image (8.13 - 2.9 MHz)
        const double beta = 7.86, i0b = besselI0(beta);
        const double fc = 0.5 * std::min(1.0, ratio) * 0.97;
        bank_.assign((size_t)L_ * kTaps, 0.f);
        for (int p = 0; p < L_; p++) {
            const double tau = (double)p / L_;
            double sum = 0;
            double t[kTaps];
            for (int k = 0; k < kTaps; k++) {
                const double x = (k - (kHalf - 1)) - tau;
                const double w = std::fabs(x) < kHalf ? besselI0(beta * std::sqrt(1.0 - (x / kHalf) * (x / kHalf))) / i0b : 0.0;
                const double sc = x == 0 ? 1.0 : std::sin(2 * M_PI * fc * x) / (2 * M_PI * fc * x);
                t[k] = 2 * fc * sc * w;
                sum += t[k];
            }
            for (int k = 0; k < kTaps; k++) bank_[(size_t)p * kTaps + k] = (float)(t[k] / sum);
        }
        reset();
        return true;
    }
    void reset() {
        re_.assign(kHalf + 2, 0.f);   // silence in front of the first sample
        im_.assign(re_.size(), 0.f);
        i0_ = (long)re_.size(); phase_ = 0;
    }
    // appends the resampled samples to out
    void process(const cf32* in, size_t n, std::vector<cf32>& out) {
        const size_t oldSize = re_.size();
        re_.resize(oldSize + n);
        im_.resize(oldSize + n);
        for (size_t i = 0; i < n; i++) { re_[oldSize + i] = in[i].real(); im_[oldSize + i] = in[i].imag(); }
        const long avail = (long)re_.size();
        // number of outputs that fit: i0 + (phase + k M) / L - (half - 1) + taps <= avail
        for (;;) {
            const long base = i0_ - (kHalf - 1);
            if (base + kTaps > avail) break;
            // run as many outputs as fit in one go, without re-testing the bound each time
            float s[2];
            genutil::dot2(&bank_[(size_t)phase_ * kTaps], &re_[(size_t)base], &im_[(size_t)base], kTaps, s);
            out.push_back(cf32(s[0], s[1]));
            phase_ += M_;
            if (phase_ >= L_) { const int c = phase_ / L_; i0_ += c; phase_ -= c * L_; }
        }
        const long keepFrom = i0_ - (kHalf - 1);
        if (keepFrom > 4096) {
            re_.erase(re_.begin(), re_.begin() + keepFrom);
            im_.erase(im_.begin(), im_.begin() + keepFrom);
            i0_ -= keepFrom;
        }
    }
private:
    static constexpr int kHalf = 12, kTaps = 2 * kHalf;
    static double besselI0(double x) {
        double s = 1, t = 1;
        for (int k = 1; k < 40; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; }
        return s;
    }
    int L_ = 0, M_ = 1;
    long i0_ = 0;
    int phase_ = 0;
    std::vector<float> bank_, re_, im_;
};

} // namespace isdbt
} // namespace dect2
