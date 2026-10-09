#include "dect2/clock_correct.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

static double bessel0(double x) { double s = 1, t = 1; for (int k = 1; k < 40; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; }

ClockCorrector::ClockCorrector() {
    // Kaiser-windowed sinc, cut-off at half the rate: the signals it serves fill at most 0.42 of it on each side (a T2 8 MHz channel at
    // 64/7 Msps), so the images of the fractional delay start at 0.58 and the window only has to reach from 0.42 to 0.58.
    constexpr int half = kTaps / 2;
    const double beta = 7.0, i0b = bessel0(beta);
    bank_.assign((size_t)(kPhases + 1) * kTaps, 0.f);
    for (int p = 0; p <= kPhases; p++) {
        const double tau = (double)p / kPhases;
        double t[kTaps], sum = 0;
        for (int k = 0; k < kTaps; k++) {
            const double x = (k - (half - 1)) - tau;
            const double r = x / (half + 0.5);
            const double w = std::fabs(r) < 1 ? bessel0(beta * std::sqrt(1.0 - r * r)) / i0b : 0.0;
            t[k] = (x == 0 ? 1.0 : std::sin(M_PI * x) / (M_PI * x)) * w;
            sum += t[k];
        }
        for (int k = 0; k < kTaps; k++) bank_[(size_t)p * kTaps + k] = (float)(t[k] / sum);
    }
    prime(nullptr, 0);
}

void ClockCorrector::prime(const cf32* hist, size_t n) {
    const size_t need = kTaps / 2;
    const size_t pad = n < need ? need - n : 0;
    re_.assign(pad, 0.f); im_.assign(pad, 0.f);
    for (size_t i = 0; i < n; i++) { re_.push_back(hist[i].real()); im_.push_back(hist[i].imag()); }
    pos_ = (double)re_.size();
}

void ClockCorrector::process(const cf32* in, size_t n, std::vector<cf32>& out) {
    if (!n) return;
    const size_t old = re_.size();
    re_.resize(old + n); im_.resize(old + n);
    for (size_t i = 0; i < n; i++) { re_[old + i] = in[i].real(); im_[old + i] = in[i].imag(); }
    const long avail = (long)re_.size();
    const float* R = re_.data();
    const float* I = im_.data();
    const float* B = bank_.data();
    out.reserve(out.size() + (size_t)((double)n / step_) + 2);
    double pos = pos_;
    for (;;) {
        const long i0 = (long)pos;
        const long base = i0 - (kTaps / 2 - 1);
        if (base + kTaps > avail) break;
        const int p = (int)((pos - (double)i0) * kPhases + 0.5);
        const float* h = B + (size_t)p * kTaps;
        const float* r = R + base;
        const float* m = I + base;
        float ar[8] = {}, ai[8] = {};   // eight lanes, so that the compiler can use vector registers without reordering a float sum
        for (int t = 0; t < kTaps; t += 8)
            for (int j = 0; j < 8; j++) { ar[j] += r[t + j] * h[t + j]; ai[j] += m[t + j] * h[t + j]; }
        out.emplace_back(((ar[0] + ar[4]) + (ar[1] + ar[5])) + ((ar[2] + ar[6]) + (ar[3] + ar[7])),
                         ((ai[0] + ai[4]) + (ai[1] + ai[5])) + ((ai[2] + ai[6]) + (ai[3] + ai[7])));
        pos += step_;
    }
    const long keepFrom = (long)pos - (kTaps / 2 - 1);
    if (keepFrom > 4096) {
        re_.erase(re_.begin(), re_.begin() + keepFrom);
        im_.erase(im_.begin(), im_.begin() + keepFrom);
        pos -= (double)keepFrom;
    }
    pos_ = pos;
}

} // namespace dect2
