// Mixed-radix FFT (see dtmb_fft.h). Decimation in time in Stockham form: a level of radix r with sub-length m (n = r m) and stride s reads
// in[q + s (r p + k)] for k < r, multiplies by exp(-j 2 pi p k / n), takes the r point DFT and writes out[q + s (p + m j)]. The levels are
// applied from the one with the largest stride (the first radix to be computed) to the one with stride 1, which produces the natural order.
#include "dect2/dtmb_fft.h"
#include <cmath>
#include <stdexcept>
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
#include <arm_neon.h>
#endif

namespace dect2::dtmb {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// r point DFT on split values (forward sign), values held in small arrays so that the compiler keeps them in registers
template <int R>
struct Dft;

template <>
struct Dft<2> {
    static inline void run(const float* xr, const float* xi, float* yr, float* yi) {
        yr[0] = xr[0] + xr[1]; yi[0] = xi[0] + xi[1];
        yr[1] = xr[0] - xr[1]; yi[1] = xi[0] - xi[1];
    }
};

template <>
struct Dft<4> {
    static inline void run(const float* xr, const float* xi, float* yr, float* yi) {
        const float s0r = xr[0] + xr[2], s0i = xi[0] + xi[2], s1r = xr[0] - xr[2], s1i = xi[0] - xi[2];
        const float s2r = xr[1] + xr[3], s2i = xi[1] + xi[3], dr = xr[1] - xr[3], di = xi[1] - xi[3];
        yr[0] = s0r + s2r; yi[0] = s0i + s2i;
        yr[2] = s0r - s2r; yi[2] = s0i - s2i;
        yr[1] = s1r + di; yi[1] = s1i - dr;   // s1 - j d
        yr[3] = s1r - di; yi[3] = s1i + dr;   // s1 + j d
    }
};

template <int R>
struct OddCoef {
    float c[R / 2 + 1][R / 2 + 1], s[R / 2 + 1][R / 2 + 1];
    OddCoef() {
        for (int m = 1; m <= R / 2; m++) for (int k = 1; k <= R / 2; k++) {
            const double a = kTwoPi * k * m / R;
            c[m][k] = (float)std::cos(a); s[m][k] = (float)std::sin(a);
        }
    }
};

template <int R>
inline const OddCoef<R>& coef() { static const OddCoef<R> c; return c; }

// odd radix (3, 5, 7): out[m] = x0 + sum_k (x_k + x_{R-k}) cos(2 pi k m / R) - j sum_k (x_k - x_{R-k}) sin(2 pi k m / R)
template <int R>
struct DftOdd {
    static inline void run(const float* xr, const float* xi, float* yr, float* yi, const OddCoef<R>& t) {
        constexpr int H = R / 2;
        float sr[H + 1], si[H + 1], dr[H + 1], di[H + 1];
        float tr = xr[0], ti = xi[0];
        for (int k = 1; k <= H; k++) {
            sr[k] = xr[k] + xr[R - k]; si[k] = xi[k] + xi[R - k];
            dr[k] = xr[k] - xr[R - k]; di[k] = xi[k] - xi[R - k];
            tr += sr[k]; ti += si[k];
        }
        yr[0] = tr; yi[0] = ti;
        for (int m = 1; m <= H; m++) {
            float ar = xr[0], ai = xi[0], br = 0.f, bi = 0.f;
            for (int k = 1; k <= H; k++) {
                ar += t.c[m][k] * sr[k]; ai += t.c[m][k] * si[k];
                br += t.s[m][k] * dr[k]; bi += t.s[m][k] * di[k];
            }
            yr[m] = ar + bi; yi[m] = ai - br;
            yr[R - m] = ar - bi; yi[R - m] = ai + br;
        }
    }
};

template <int R>
inline void butterfly(const float* xr, const float* xi, float* yr, float* yi) {
    if constexpr (R == 2) Dft<2>::run(xr, xi, yr, yi);
    else if constexpr (R == 4) Dft<4>::run(xr, xi, yr, yi);
    else DftOdd<R>::run(xr, xi, yr, yi, coef<R>());
}

// One level: p < m, q < s
template <int R>
void level(const float* inr, const float* ini, float* outr, float* outi, int m, int s, const float* twr, const float* twi) {
    for (int p = 0; p < m; p++) {
        float wr[R], wi[R];
        wr[0] = 1.f; wi[0] = 0.f;
        for (int k = 1; k < R; k++) { wr[k] = twr[(k - 1) * m + p]; wi[k] = twi[(k - 1) * m + p]; }
        const float* ir[R];
        const float* ii[R];
        float* orr[R];
        float* oi[R];
        for (int k = 0; k < R; k++) {
            ir[k] = inr + (size_t)s * (size_t)(R * p + k); ii[k] = ini + (size_t)s * (size_t)(R * p + k);
            orr[k] = outr + (size_t)s * (size_t)(p + m * k); oi[k] = outi + (size_t)s * (size_t)(p + m * k);
        }
        for (int q = 0; q < s; q++) {
            float xr[R], xi[R], yr[R], yi[R];
            xr[0] = ir[0][q]; xi[0] = ii[0][q];
            for (int k = 1; k < R; k++) {
                const float a = ir[k][q], b = ii[k][q];
                xr[k] = a * wr[k] - b * wi[k];
                xi[k] = a * wi[k] + b * wr[k];
            }
            butterfly<R>(xr, xi, yr, yi);
            for (int k = 0; k < R; k++) { orr[k][q] = yr[k]; oi[k][q] = yi[k]; }
        }
    }
}

void applyLevel(int r, const float* inr, const float* ini, float* outr, float* outi, int m, int s, const float* twr, const float* twi) {
    switch (r) {
    case 2: level<2>(inr, ini, outr, outi, m, s, twr, twi); break;
    case 3: level<3>(inr, ini, outr, outi, m, s, twr, twi); break;
    case 4: level<4>(inr, ini, outr, outi, m, s, twr, twi); break;
    case 5: level<5>(inr, ini, outr, outi, m, s, twr, twi); break;
    default: level<7>(inr, ini, outr, outi, m, s, twr, twi); break;
    }
}

} // namespace

MixedFft::MixedFft(int n) : n_(n) {
    if (n < 1) throw std::invalid_argument("MixedFft: size");
    // radices from the top level (stride 1) down: a 4 first so that the last pass is the cheapest and runs over p, then the rest
    std::vector<int> top;
    int rest = n;
    while (rest % 4 == 0) { top.push_back(4); rest /= 4; break; }
    while (rest % 2 == 0) { top.push_back(2); rest /= 2; }
    while (rest % 3 == 0) { top.push_back(3); rest /= 3; }
    while (rest % 5 == 0) { top.push_back(5); rest /= 5; }
    while (rest % 7 == 0) { top.push_back(7); rest /= 7; }
    if (rest != 1) throw std::invalid_argument("MixedFft: prime factors other than 2, 3, 5, 7");
    int len = n, s = 1;
    std::vector<Level> fromTop;
    for (int r : top) {
        Level L;
        L.r = r; L.n = len; L.m = len / r; L.s = s;
        L.twr.resize((size_t)(r - 1) * (size_t)L.m); L.twi.resize(L.twr.size());
        for (int k = 1; k < r; k++) for (int p = 0; p < L.m; p++) {
            const double a = -kTwoPi * (double)p * (double)k / (double)len;
            L.twr[(size_t)((k - 1) * L.m + p)] = (float)std::cos(a);
            L.twi[(size_t)((k - 1) * L.m + p)] = (float)std::sin(a);
        }
        fromTop.push_back(std::move(L));
        s *= r; len /= r;
    }
    levels_.assign(fromTop.rbegin(), fromTop.rend());   // the deepest level is computed first
    ar_.resize((size_t)n); ai_.resize((size_t)n); br_.resize((size_t)n); bi_.resize((size_t)n);
}

namespace {
// The last level (radix 4, stride 1) writes interleaved complex values straight into x. sg = -1 conjugates (inverse transform).
void lastLevel4(const float* inr, const float* ini, cf32* x, int m, const float* twr, const float* twi, float sg) {
    float* xf = reinterpret_cast<float*>(x);
    int p = 0;
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
    const float32x4_t sgv = vdupq_n_f32(sg);
    for (; p + 4 <= m; p += 4) {
        const float32x4x4_t a = vld4q_f32(inr + 4 * p), b = vld4q_f32(ini + 4 * p);
        float32x4_t xr[4], xi[4];
        xr[0] = a.val[0]; xi[0] = b.val[0];
        for (int k = 1; k < 4; k++) {
            const float32x4_t wr = vld1q_f32(twr + (k - 1) * m + p), wi = vld1q_f32(twi + (k - 1) * m + p);
            xr[k] = vsubq_f32(vmulq_f32(a.val[k], wr), vmulq_f32(b.val[k], wi));
            xi[k] = vaddq_f32(vmulq_f32(a.val[k], wi), vmulq_f32(b.val[k], wr));
        }
        const float32x4_t s0r = vaddq_f32(xr[0], xr[2]), s0i = vaddq_f32(xi[0], xi[2]), s1r = vsubq_f32(xr[0], xr[2]), s1i = vsubq_f32(xi[0], xi[2]);
        const float32x4_t s2r = vaddq_f32(xr[1], xr[3]), s2i = vaddq_f32(xi[1], xi[3]), dr = vsubq_f32(xr[1], xr[3]), di = vsubq_f32(xi[1], xi[3]);
        float32x4_t yr[4], yi[4];
        yr[0] = vaddq_f32(s0r, s2r); yi[0] = vaddq_f32(s0i, s2i);
        yr[2] = vsubq_f32(s0r, s2r); yi[2] = vsubq_f32(s0i, s2i);
        yr[1] = vaddq_f32(s1r, di); yi[1] = vsubq_f32(s1i, dr);
        yr[3] = vsubq_f32(s1r, di); yi[3] = vaddq_f32(s1i, dr);
        for (int j = 0; j < 4; j++) {
            float32x4x2_t o; o.val[0] = yr[j]; o.val[1] = vmulq_f32(yi[j], sgv);
            vst2q_f32(xf + 2 * (p + m * j), o);
        }
    }
#endif
    for (; p < m; p++) {
        float xr[4], xi[4], yr[4], yi[4];
        xr[0] = inr[4 * p]; xi[0] = ini[4 * p];
        for (int k = 1; k < 4; k++) {
            const float a = inr[4 * p + k], b = ini[4 * p + k], wr = twr[(k - 1) * m + p], wi = twi[(k - 1) * m + p];
            xr[k] = a * wr - b * wi; xi[k] = a * wi + b * wr;
        }
        Dft<4>::run(xr, xi, yr, yi);
        for (int j = 0; j < 4; j++) x[p + m * j] = cf32(yr[j], sg * yi[j]);
    }
}
} // namespace

void MixedFft::run(cf32* x, bool inv) {
    const int N = n_;
    // split the input (the inverse transform is the forward one of the conjugate)
    const float sg = inv ? -1.f : 1.f;
    {
        const float* xf = reinterpret_cast<const float*>(x);
        int i = 0;
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
        const float32x4_t sgv = vdupq_n_f32(sg);
        for (; i + 4 <= N; i += 4) {
            const float32x4x2_t v = vld2q_f32(xf + 2 * i);
            vst1q_f32(&ar_[(size_t)i], v.val[0]);
            vst1q_f32(&ai_[(size_t)i], vmulq_f32(v.val[1], sgv));
        }
#endif
        for (; i < N; i++) { ar_[(size_t)i] = xf[2 * i]; ai_[(size_t)i] = sg * xf[2 * i + 1]; }
    }
    float* ir = ar_.data(); float* ii = ai_.data();
    float* orr = br_.data(); float* oi = bi_.data();
    const size_t last = levels_.size() - 1;
    for (size_t li = 0; li < levels_.size(); li++) {
        const Level& L = levels_[li];
        if (li == last && L.r == 4 && L.s == 1) { lastLevel4(ir, ii, x, L.m, L.twr.data(), L.twi.data(), sg); return; }
        applyLevel(L.r, ir, ii, orr, oi, L.m, L.s, L.twr.data(), L.twi.data());
        std::swap(ir, orr); std::swap(ii, oi);
    }
    for (int i = 0; i < N; i++) x[i] = cf32(ir[i], sg * ii[i]);
}

} // namespace dect2::dtmb
