#include "atsc3_synth_resampler.h"
#include "dect2/gen_util.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace atsc3synth {

namespace {
double besselI0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 40; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; }
    return s;
}
}

bool FastResampler::configure(double inRate, double outRate) {
    if (inRate <= 0 || outRate < 0.9 * inRate || outRate > 4 * inRate) return false;
    step_ = inRate / outRate;
    pass_ = std::fabs(outRate / inRate - 1.0) < 1e-12;
    constexpr int half = kTaps / 2;
    if (!pass_) {
        bank_.assign((size_t)(kPhases + 1) * kTaps, 0.f);
        const double beta = 5.8, i0b = besselI0(beta), fc = 0.5;
        for (int p = 0; p <= kPhases; p++) {
            const double tau = (double)p / kPhases;
            double t[kTaps], sum = 0;
            for (int k = 0; k < kTaps; k++) {
                const double x = (k - (half - 1)) - tau;   // distance of tap k from the output position, in input samples
                const double w = std::fabs(x) < half ? besselI0(beta * std::sqrt(1.0 - (x / half) * (x / half))) / i0b : 0.0;
                const double sc = x == 0 ? 1.0 : std::sin(2 * M_PI * fc * x) / (2 * M_PI * fc * x);
                t[k] = 2 * fc * sc * w;
                sum += t[k];
            }
            for (int k = 0; k < kTaps; k++) bank_[(size_t)p * kTaps + k] = (float)(t[k] / sum);
        }
    }
    re_.assign(half + 2, 0.f);   // silence in front of the first sample
    im_.assign(re_.size(), 0.f);
    pos_ = (double)re_.size();
    return true;
}

void FastResampler::process(const cf32* in, size_t n, std::vector<cf32>& out) {
    if (!n) return;
    if (pass_) { out.insert(out.end(), in, in + n); return; }
    constexpr int half = kTaps / 2;
    const size_t old = re_.size();
    re_.resize(old + n);
    im_.resize(old + n);
    for (size_t i = 0; i < n; i++) { re_[old + i] = in[i].real(); im_[old + i] = in[i].imag(); }
    const long avail = (long)re_.size();
    // the outputs whose whole window lies inside the buffer
    const double last = (double)(avail - kTaps) + (half - 1);   // the largest pos_ with base + kTaps <= avail
    if (pos_ > last) return;
    const size_t count = (size_t)std::floor((last - pos_) / step_) + 1;
    const size_t at = out.size();
    out.resize(at + count);
    cf32* o = out.data() + at;
    const float* re = re_.data();
    const float* im = im_.data();
    const float* bank = bank_.data();
    double pos = pos_;
    for (size_t i = 0; i < count; i++) {
        const long i0 = (long)pos;
        const int p = (int)((pos - (double)i0) * kPhases + 0.5);
        float r[2];
        genutil::dot2(bank + (size_t)p * kTaps, re + (i0 - (half - 1)), im + (i0 - (half - 1)), kTaps, r);
        o[i] = cf32(r[0], r[1]);
        pos += step_;
    }
    pos_ = pos;
    const long keepFrom = (long)std::floor(pos_) - (half - 1);
    if (keepFrom > 0) {
        re_.erase(re_.begin(), re_.begin() + keepFrom);
        im_.erase(im_.begin(), im_.begin() + keepFrom);
        pos_ -= (double)keepFrom;
    }
}

} // namespace atsc3synth
} // namespace dect2
