#include "dect2/exact_resampler.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

namespace {
double besselI0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 40; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; }
    return s;
}
}

bool ExactResampler::configure(double inRate, double outRate) {
    if (inRate <= 0 || outRate <= 0 || outRate > 4 * inRate) return false;
    const double ratio = outRate / inRate;
    step_ = inRate / outRate;
    pass_ = std::fabs(ratio - 1.0) < 1e-12;
    if (pass_) { reset(); return true; }
    const double down = std::min(1.0, ratio);
    const double fc = 0.5 * down * 0.97;
    half_ = (int)std::ceil(16.0 / down);
    taps_ = 2 * half_;
    stride_ = (taps_ + 3) & ~3;
    bank_.assign((size_t)(phases_ + 1) * (size_t)stride_, 0.f);
    const double beta = 8.0, i0b = besselI0(beta);
    std::vector<double> t((size_t)taps_);
    for (int p = 0; p <= phases_; p++) {
        const double tau = (double)p / phases_;
        double sum = 0;
        for (int k = 0; k < taps_; k++) {
            const double x = (k - (half_ - 1)) - tau;
            const double w = std::fabs(x) < half_ ? besselI0(beta * std::sqrt(1.0 - (x / half_) * (x / half_))) / i0b : 0.0;
            const double sc = x == 0 ? 1.0 : std::sin(2 * M_PI * fc * x) / (2 * M_PI * fc * x);
            t[(size_t)k] = 2 * fc * sc * w;
            sum += t[(size_t)k];
        }
        for (int k = 0; k < taps_; k++) bank_[(size_t)p * (size_t)stride_ + (size_t)k] = (float)(t[(size_t)k] / sum);
    }
    reset();
    return true;
}

void ExactResampler::reset() {
    re_.assign((size_t)(half_ + 2), 0.f);   // history of silence in front of the first sample
    im_.assign(re_.size(), 0.f);
    pos_ = (double)re_.size();               // the first output sits on the first input sample
}

void ExactResampler::process(const cf32* in, size_t n, std::vector<cf32>& out) {
    if (!n) return;
    if (pass_) { out.insert(out.end(), in, in + n); return; }
    for (size_t i = 0; i < n; i++) { re_.push_back(in[i].real()); im_.push_back(in[i].imag()); }
    const long avail = (long)re_.size();
    long produced = 0;
    for (;;) {
        const long i0 = (long)std::floor(pos_);
        const long base = i0 - (half_ - 1);
        if (base + taps_ > avail) break;
        const int p = (int)std::lround((pos_ - (double)i0) * phases_);
        const float* h = &bank_[(size_t)p * (size_t)stride_];
        const float* r = &re_[(size_t)base];
        const float* m = &im_[(size_t)base];
        float ar[4] = {0, 0, 0, 0}, ai[4] = {0, 0, 0, 0};
        for (int t = 0; t < stride_; t += 4)
            for (int j = 0; j < 4; j++) { ar[j] += r[t + j] * h[t + j]; ai[j] += m[t + j] * h[t + j]; }
        out.push_back(cf32((ar[0] + ar[1]) + (ar[2] + ar[3]), (ai[0] + ai[1]) + (ai[2] + ai[3])));
        pos_ += step_;
        produced++;
    }
    // drop what is no longer needed, keeping the history the next output needs
    const long keepFrom = (long)std::floor(pos_) - (half_ - 1);
    if (keepFrom > 4096) {
        re_.erase(re_.begin(), re_.begin() + keepFrom);
        im_.erase(im_.begin(), im_.begin() + keepFrom);
        pos_ -= (double)keepFrom;
    }
    (void)produced;
}

} // namespace dect2
