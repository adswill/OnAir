// Mixed radix decimation in time FFT (radix 4, 2, 3, 5 and a general odd radix for the rest).
#include "dect2/drm_fft.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 { namespace drm {

DrmFft::DrmFft(int n) : n_(n < 1 ? 1 : n) {
    int rest = n_;
    // radix 4 first (fewer passes), then 2, 3, 5, and any other prime
    for (int p : {4, 2, 3, 5}) while (rest % p == 0) { rest /= p; factors_.push_back(p); factors_.push_back(0); }
    for (int p = 7; rest > 1; p += 2) while (rest % p == 0) { rest /= p; factors_.push_back(p); factors_.push_back(0); }
    int m = n_;
    for (size_t i = 0; i < factors_.size(); i += 2) { m /= factors_[i]; factors_[i + 1] = m; }
    tw_.resize((size_t)n_);
    for (int i = 0; i < n_; i++) {
        const double a = -2.0 * M_PI * (double)i / (double)n_;
        tw_[(size_t)i] = cf32((float)std::cos(a), (float)std::sin(a));
    }
    int maxp = 2;
    for (size_t i = 0; i < factors_.size(); i += 2) maxp = std::max(maxp, factors_[i]);
    scratch_.resize((size_t)maxp);
    tmp_.resize((size_t)n_);
}

void DrmFft::run(cf32* x, bool inv) const {
    if (n_ == 1) return;
    std::memcpy(tmp_.data(), x, sizeof(cf32) * (size_t)n_);
    work(x, tmp_.data(), 1, factors_.data(), inv);
}

void DrmFft::work(cf32* out, const cf32* in, size_t fstride, const int* factors, bool inv) const {
    const int p = factors[0], m = factors[1];
    cf32* const end = out + (size_t)p * (size_t)m;
    if (m == 1) {
        for (cf32* o = out; o != end; ++o, in += fstride) *o = *in;
    } else {
        for (cf32* o = out; o != end; o += m, in += fstride) work(o, in, fstride * (size_t)p, factors + 2, inv);
    }
    const size_t N = (size_t)n_;
    auto tw = [&](size_t idx) { const cf32 t = tw_[idx % N]; return inv ? std::conj(t) : t; };
    if (p == 2) {
        for (int u = 0; u < m; u++) {
            const cf32 t = out[u + m] * tw((size_t)u * fstride);
            out[u + m] = out[u] - t;
            out[u] += t;
        }
    } else if (p == 4) {
        for (int u = 0; u < m; u++) {
            const cf32 a0 = out[u];
            const cf32 a1 = out[u + m] * tw((size_t)u * fstride);
            const cf32 a2 = out[u + 2 * m] * tw((size_t)u * 2 * fstride);
            const cf32 a3 = out[u + 3 * m] * tw((size_t)u * 3 * fstride);
            const cf32 s02 = a0 + a2, d02 = a0 - a2, s13 = a1 + a3, d13 = a1 - a3;
            // multiplication by -j (forward) or +j (inverse)
            const cf32 jd = inv ? cf32(-d13.imag(), d13.real()) : cf32(d13.imag(), -d13.real());
            out[u] = s02 + s13;
            out[u + m] = d02 + jd;
            out[u + 2 * m] = s02 - s13;
            out[u + 3 * m] = d02 - jd;
        }
    } else {
        cf32* s = scratch_.data();
        const size_t step = (size_t)m * fstride;   // N / p
        for (int u = 0; u < m; u++) {
            for (int q = 0; q < p; q++) s[q] = out[u + q * m] * tw((size_t)u * (size_t)q * fstride);
            for (int k = 0; k < p; k++) {
                cf32 acc = s[0];
                for (int q = 1; q < p; q++) acc += s[q] * tw(step * (size_t)((q * k) % p));
                out[u + k * m] = acc;
            }
        }
    }
}

}} // namespace dect2::drm
