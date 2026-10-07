// DVB-S2 receiver: decisions and phase refinement, see dvbs_s2refine.h.
#include "dvbs_s2refine.h"
#include "dect2/dvbs_s2.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

namespace dect2 {
namespace dvbs {

void NearestPoint::build(const cf32* p, int count) {
    pts_ = p; n_ = count;
    float mx = 0.f;
    for (int i = 0; i < count; i++) mx = std::max(mx, std::abs(p[i]));
    lim_ = std::max(1.5f, mx * 1.35f);
    scale_ = (float)kGrid / (2.f * lim_);
    idx_.assign((size_t)kGrid * kGrid, 0);
    idx2_.assign((size_t)kGrid * kGrid, 0);
    for (int iy = 0; iy < kGrid; iy++)
        for (int ix = 0; ix < kGrid; ix++) {
            const float re = ((float)ix + 0.5f) / scale_ - lim_, im = ((float)iy + 0.5f) / scale_ - lim_;
            float bd = 1e30f, bd2 = 1e30f; int bi = 0, bi2 = 0;
            for (int i = 0; i < count; i++) {
                const float a = re - p[i].real(), b = im - p[i].imag(), d = a * a + b * b;
                if (d < bd) { bd2 = bd; bi2 = bi; bd = d; bi = i; }
                else if (d < bd2) { bd2 = d; bi2 = i; }
            }
            idx_[(size_t)iy * kGrid + (size_t)ix] = (uint8_t)bi;
            idx2_[(size_t)iy * kGrid + (size_t)ix] = (uint8_t)bi2;
        }
}

const NearestPoint& nearestPoint(int mod, int rate) {
    static std::mutex mu;
    static std::map<int, NearestPoint*> cache;
    std::lock_guard<std::mutex> lk(mu);
    const int key = mod < 2 ? mod * 100 : mod * 100 + rate;
    auto it = cache.find(key);
    if (it != cache.end()) return *it->second;
    auto* np = new NearestPoint();          // kept for the life of the program: callers keep references
    np->build(s2Constellation(mod, rate), s2ConstellationSize(mod));
    cache[key] = np;
    return *np;
}

namespace {
inline float fexpNeg(float x) {        // e^x for x <= 0
    x = std::max(x, -60.f);
    const float t = x * 1.44269504f;
    const float fi = std::floor(t), f = t - fi;
    const float p = 1.f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * 0.00961813f)));
    union { float f; int32_t i; } u;
    u.i = ((int32_t)fi + 127) << 23;
    return p * u.f;
}
inline float ftanhf(float x) {
    const float ax = std::min(std::fabs(x), 12.f);
    const float e = fexpNeg(-2.f * ax);
    const float t = (1.f - e) / (1.f + e);
    return x < 0 ? -t : t;
}
} // namespace

float s2RefinePhase(cf32* x, int n, int mod, int rate, float sigma2, int W, const std::vector<PhaseMark>& marks, int iterations) {
    (void)marks;
    if (n < 4 * W || W < 4) return 0.f;
    const cf32* c = s2Constellation(mod, rate);
    const int P = s2ConstellationSize(mod);
    const NearestPoint& np = nearestPoint(mod, rate);
    float dmin = 1e9f;
    for (int i = 0; i < P; i++) for (int j = i + 1; j < P; j++) dmin = std::min(dmin, std::abs(c[i] - c[j]));
    const bool soft = sigma2 > (dmin * 0.125f) * (dmin * 0.125f);
    const float invS = 1.f / std::max(sigma2, 1e-4f);
    const int nb = n / W;
    std::vector<cf32> acc((size_t)nb), sm((size_t)nb);
    std::vector<float> eps((size_t)n);
    double sq = 0;
    for (int it = 0; it < iterations; it++) {
        for (int b = 0; b < nb; b++) {
            const int k0 = b * W, k1 = b == nb - 1 ? n : k0 + W;
            float ar = 0, ai = 0;
            for (int k = k0; k < k1; k++) {
                const float xr = x[k].real(), xi = x[k].imag();
                float er, ei;
                if (!soft) { const cf32 e = c[np.index(xr, xi)]; er = e.real(); ei = e.imag(); }
                else if (mod == kQpsk) { er = 0.70710678f * ftanhf(0.70710678f * invS * xr); ei = 0.70710678f * ftanhf(0.70710678f * invS * xi); }
                else {
                    float w[32], mx = -1e30f;
                    const float h = 0.5f * invS;
                    for (int i = 0; i < P; i++) { const float a = xr - c[i].real(), bb = xi - c[i].imag(); w[i] = -(a * a + bb * bb) * h; mx = std::max(mx, w[i]); }
                    float sw = 0, sr = 0, si = 0;
                    for (int i = 0; i < P; i++) { const float e = fexpNeg(w[i] - mx); sw += e; sr += e * c[i].real(); si += e * c[i].imag(); }
                    er = sr / sw; ei = si / sw;
                }
                ar += xr * er + xi * ei;           // x conj(E)
                ai += xi * er - xr * ei;
            }
            acc[(size_t)b] = cf32(ar, ai);
        }
        // zero lag smoothing across the blocks: a 1 2 1 weighted vector sum (the weights are the block magnitudes, so unreliable blocks count little)
        for (int b = 0; b < nb; b++) {
            cf32 s = 2.f * acc[(size_t)b];
            if (b > 0) s += acc[(size_t)b - 1];
            if (b + 1 < nb) s += acc[(size_t)b + 1];
            sm[(size_t)b] = s;
        }
        // phase per block centre, linear interpolation in between (the phase is unwrapped along the way)
        std::vector<float> ph((size_t)nb);
        float prev = 0;
        for (int b = 0; b < nb; b++) {
            float a = std::abs(sm[(size_t)b]) > 1e-9f ? std::arg(sm[(size_t)b]) : prev;
            while (a - prev > 3.14159265f) a -= 6.2831853f;
            while (a - prev < -3.14159265f) a += 6.2831853f;
            ph[(size_t)b] = prev = a;
        }
        // the correction is linear between block centres: a rotor with a constant step inside each stretch, renewed at the stretch start
        {
            int k = 0;
            while (k < n) {
                const float t = ((float)k - 0.5f * (float)W) / (float)W;
                int b0 = (int)std::floor(t);
                int b1 = b0 + 1;
                int kEnd;
                float e0, slope;
                if (b0 < 0) { e0 = ph[0]; slope = 0.f; kEnd = std::min(n, W / 2); }
                else if (b1 >= nb) { e0 = ph[(size_t)nb - 1]; slope = 0.f; kEnd = n; }
                else {
                    const float f = t - (float)b0;
                    e0 = ph[(size_t)b0] * (1.f - f) + ph[(size_t)b1] * f;
                    slope = (ph[(size_t)b1] - ph[(size_t)b0]) / (float)W;
                    kEnd = std::min(n, b1 * W + W / 2);
                }
                if (kEnd <= k) kEnd = k + 1;
                cf32 rot(std::cos(e0), -std::sin(e0));
                const cf32 stp(std::cos(slope), -std::sin(slope));
                for (int j = k; j < kEnd; j++) { eps[(size_t)j] = e0 + slope * (float)(j - k); x[j] *= rot; rot *= stp; }
                k = kEnd;
            }
        }
        if (it == iterations - 1) for (int k = 0; k < n; k++) sq += (double)eps[(size_t)k] * eps[(size_t)k];
    }
    return (float)std::sqrt(sq / n);
}

} // namespace dvbs
} // namespace dect2
