// The portable DSP kernels (FFT, filters, window, small BLAS) against textbook implementations.
#include "dect2/dsp_compat.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
#include <algorithm>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    std::mt19937 rng(2);
    std::normal_distribution<float> nd(0, 1);
    // FFT vs the DFT definition, forward and inverse, several sizes
    for (int lg = 1; lg <= 12; lg++) {
        const int n = 1 << lg;
        std::vector<float> re(n), im(n);
        for (int i = 0; i < n; i++) { re[i] = nd(rng); im[i] = nd(rng); }
        for (int inv = 0; inv < 2; inv++) {
            std::vector<float> r = re, m = im;
            fftSplit(r.data(), m.data(), lg, inv);
            double worst = 0, scale = 0;
            const int probes = std::min(n, 48);
            for (int pk = 0; pk < probes; pk++) {
                const int k = probes == n ? pk : (int)(((long long)pk * 7919) % n);
                double sr = 0, si = 0;
                for (int t = 0; t < n; t++) {
                    const double a = (inv ? 2.0 : -2.0) * M_PI * (double)((long long)k * t % n) / n;
                    sr += re[t] * std::cos(a) - im[t] * std::sin(a);
                    si += re[t] * std::sin(a) + im[t] * std::cos(a);
                }
                worst = std::max(worst, std::hypot(sr - r[k], si - m[k]));
                scale = std::max(scale, std::hypot(sr, si));
            }
            CHECK(worst < 2e-5 * scale + 1e-4, "FFT size %d %s: error %.3g (scale %.3g)", n, inv ? "inverse" : "forward", worst, scale);
        }
        // round trip
        std::vector<float> r = re, m = im;
        fftSplit(r.data(), m.data(), lg, false);
        fftSplit(r.data(), m.data(), lg, true);
        double e = 0;
        for (int i = 0; i < n; i++) e = std::max(e, (double)std::hypot(r[i] / n - re[i], m[i] / n - im[i]));
        CHECK(e < 1e-4, "FFT round trip size %d: %.3g", n, e);
    }
    // correlation with an output stride, decimating filter
    {
        const int N = 100, P = 37, S = 3;
        std::vector<float> a(N + P + 8), f(P), c((size_t)N * S + 3, -9.f);
        for (auto& v : a) v = nd(rng);
        for (auto& v : f) v = nd(rng);
        convCorr(a.data(), f.data(), c.data(), S, N, P);
        double e = 0;
        for (int i = 0; i < N; i++) { double s = 0; for (int k = 0; k < P; k++) s += f[k] * a[i + k]; e = std::max(e, std::fabs(s - c[(size_t)i * S])); }
        CHECK(e < 1e-4, "convCorr error %.3g", e);
        CHECK(c[1] == -9.f && c[2] == -9.f, "convCorr wrote between the strided outputs");
        std::vector<float> d(N);
        const int D = 5;
        std::vector<float> a2((size_t)N * D + P + 8);
        for (auto& v : a2) v = nd(rng);
        desamp(a2.data(), D, f.data(), d.data(), N, P);
        e = 0;
        for (int i = 0; i < N; i++) { double s = 0; for (int k = 0; k < P; k++) s += f[k] * a2[(size_t)i * D + k]; e = std::max(e, std::fabs(s - d[i])); }
        CHECK(e < 1e-4, "desamp error %.3g", e);
    }
    // Hann window (unit rms)
    {
        const int n = 1024;
        std::vector<float> w(n);
        hannWindowNorm(w.data(), n);
        double p = 0;
        for (float v : w) p += (double)v * v;
        CHECK(std::fabs(p / n - 1.0) < 1e-3, "Hann rms^2 %.4f", p / n);
        CHECK(std::fabs(w[0]) < 1e-6 && std::fabs(w[n / 2] - std::sqrt(8.0 / 3.0)) < 1e-4, "Hann shape");
    }
    // x^T x and x^T t
    {
        const int rows = 300, n = 41;
        std::vector<float> x((size_t)rows * n), t(rows), A((size_t)n * n), b(n);
        for (auto& v : x) v = nd(rng);
        for (auto& v : t) v = nd(rng);
        syrkLowerT(x.data(), rows, n, A.data());
        gemvT(x.data(), rows, n, t.data(), b.data());
        double e = 0, e2 = 0;
        for (int i = 0; i < n; i++) {
            for (int j = 0; j <= i; j++) { double s = 0; for (int r = 0; r < rows; r++) s += (double)x[(size_t)r * n + i] * x[(size_t)r * n + j]; e = std::max(e, std::fabs(s - A[(size_t)i * n + j])); }
            double s = 0; for (int r = 0; r < rows; r++) s += (double)x[(size_t)r * n + i] * t[r];
            e2 = std::max(e2, std::fabs(s - b[i]));
        }
        CHECK(e < 1e-3 && e2 < 1e-3, "syrk %.3g gemv %.3g", e, e2);
    }
    printf(fails ? "DSP tests FAILED\n" : "DSP tests passed\n");
    return fails ? 1 : 0;
}
