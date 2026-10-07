// DTMB front end (see dtmb_front.h).
#include "dect2/dtmb_front.h"
#include "dect2/dtmb_defs.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "dtmb_simd.h"

namespace dect2::dtmb {

namespace {
constexpr int kSpan = 20;      // symbols on each side of the pulse
constexpr double kBeta = 5.0;  // Kaiser window of the truncated pulse
double besselI0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 50; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; }
    return s;
}

}

bool SrrcResampler::configure(double inRate) {
    if (inRate < 7.95e6) return false;
    inRate_ = inRate;
    ratio_ = inRate / kSymbolRate;
    half_ = (int)std::ceil(kSpan * ratio_);
    taps_ = 2 * half_;
    stride_ = (taps_ + 7) & ~7;
    bank_.assign((size_t)(phases_ + 1) * (size_t)stride_, 0.f);
    const double i0b = besselI0(kBeta);
    for (int p = 0; p <= phases_; p++) {
        const double f = (double)p / phases_;
        for (int k = 0; k < taps_; k++) {
            const double d = (double)(k - half_ + 1) - f;        // input samples from the output instant
            const double t = d / ratio_;                          // symbol periods
            const double u = t / kSpan;
            const double w = std::fabs(u) < 1.0 ? besselI0(kBeta * std::sqrt(1.0 - u * u)) / i0b : 0.0;
            bank_[(size_t)p * (size_t)stride_ + (size_t)k] = (float)(srrcPulse(t) * w / ratio_);
        }
    }
    reset();
    return true;
}

void SrrcResampler::reset() {
    re_.assign((size_t)half_ + 2, 0.f);   // silence in front of the first sample
    im_.assign(re_.size(), 0.f);
    fill_ = re_.size();
    pos_ = (int64_t)std::llround((double)re_.size() * kOne);
    spacing_ = 0;
    step_ = (int64_t)std::llround(ratio_ * kOne);
    dc_re_ = dc_im_ = 0;
    dcCount_ = 0;
}

void SrrcResampler::push(const cf32* in, size_t n) {
    if (!n || taps_ == 0) return;
    // DC offset of the radio: a slow average (time constant 25 ms), a plain average at first so that it settles quickly; updated per block
    double sr = 0, si = 0;
    for (size_t i = 0; i < n; i++) { sr += in[i].real(); si += in[i].imag(); }
    const double alpha = std::max((double)n / (inRate_ * 0.025), (double)n / (double)(dcCount_ + (long)n));
    dc_re_ += std::min(1.0, alpha) * (sr / (double)n - dc_re_);
    dc_im_ += std::min(1.0, alpha) * (si / (double)n - dc_im_);
    dcCount_ += (long)n;
    const float dr = dcOn_ ? (float)dc_re_ : 0.f, di = dcOn_ ? (float)dc_im_ : 0.f;
    if (fill_ + n + (size_t)stride_ + 16 > re_.size()) { re_.resize(fill_ + n + (size_t)stride_ + 16 + 65536); im_.resize(re_.size()); }
    float* r = &re_[fill_];
    float* m = &im_[fill_];
    for (size_t i = 0; i < n; i++) { r[i] = in[i].real() - dr; m[i] = in[i].imag() - di; }
    fill_ += n;
}

size_t SrrcResampler::available() const {
    // outputs with base + stride <= fill
    const int64_t lastPos = (int64_t)(fill_ - (size_t)stride_ + (size_t)(half_ - 1)) * (int64_t)kOne;
    if (lastPos < pos_) return 0;
    return (size_t)((lastPos - pos_) / step_) + 1;
}

size_t SrrcResampler::pull(std::vector<cf32>& out, size_t maxOut) {
    if (taps_ == 0) return 0;
    size_t n = std::min(available(), maxOut);
    const size_t at = out.size();
    out.resize(at + n);
    cf32* o = out.data() + at;
    const int shift = 32 - 9;   // 512 phases
    for (size_t k = 0; k < n; k++) {
        int64_t i0 = pos_ >> 32;
        // nearest of the 512 phases; a carry moves to the next input sample
        uint32_t ph = (uint32_t)(((uint64_t)(pos_ & 0xFFFFFFFFll) + (1ull << (shift - 1))) >> shift);
        if (ph >= 512) { ph = 0; i0++; }
        const long base = (long)i0 - (half_ - 1);
        float vr, vi;
        dotRI(&re_[(size_t)base], &im_[(size_t)base], &bank_[(size_t)ph * (size_t)stride_], stride_, vr, vi);
        o[k] = cf32(vr, vi);
        pos_ += step_;
    }
    // drop what is no longer needed
    const long keepFrom = (long)(pos_ >> 32) - (half_ - 1) - 1;
    if (keepFrom > 16384) {
        const size_t d = (size_t)keepFrom;
        std::memmove(re_.data(), re_.data() + d, (fill_ - d) * sizeof(float));
        std::memmove(im_.data(), im_.data() + d, (fill_ - d) * sizeof(float));
        fill_ -= d;
        pos_ -= (int64_t)d << 32;
    }
    return n;
}

} // namespace dect2::dtmb
