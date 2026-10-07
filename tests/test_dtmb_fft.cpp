// MixedFft against a direct DFT, round trips, and speed at the DTMB size.
#include "dect2/dtmb_fft.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>

using namespace dect2;
using namespace dect2::dtmb;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main() {
    std::mt19937 rng(3);
    std::normal_distribution<float> nd;
    for (int n : {1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 15, 21, 35, 36, 49, 60, 105, 140, 210, 315, 420, 540, 945, 1260, 3780}) {
        MixedFft f(n);
        std::vector<cf32> x((size_t)n), y((size_t)n), ref((size_t)n);
        for (auto& v : x) v = cf32(nd(rng), nd(rng));
        y = x;
        f.forward(y.data());
        double err = 0, pw = 0;
        if (n <= 1260 || n == 3780) {
            for (int k = 0; k < n; k++) {
                std::complex<double> acc = 0;
                for (int t = 0; t < n; t++) acc += std::complex<double>(x[(size_t)t]) * std::polar(1.0, -6.283185307179586 * (double)((long)k * t % n) / n);
                err += std::norm(acc - std::complex<double>(y[(size_t)k]));
                pw += std::norm(acc);
            }
        }
        CHECK(std::sqrt(err / std::max(pw, 1e-30)) < 2e-6, "n=%d forward error %g", n, std::sqrt(err / std::max(pw, 1e-30)));
        f.inverse(y.data());
        double e2 = 0, p2 = 0;
        for (int k = 0; k < n; k++) { e2 += std::norm(y[(size_t)k] / (float)n - x[(size_t)k]); p2 += std::norm(x[(size_t)k]); }
        CHECK(std::sqrt(e2 / p2) < 2e-6, "n=%d round trip error %g", n, std::sqrt(e2 / p2));
    }
    bool threw = false;
    try { MixedFft bad(11); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw, "size 11 must be refused");
    {
        MixedFft f(3780);
        std::vector<cf32> x(3780);
        for (auto& v : x) v = cf32(nd(rng), nd(rng));
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 2000; i++) { f.forward(x.data()); f.inverse(x.data()); for (auto& v : x) v *= 1.f / 3780.f; }
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 4000;
        printf("  3780-point FFT: %.1f us\n", dt * 1e6);
    }
    printf(failures ? "dtmb_fft: %d FAILED\n" : "dtmb_fft: all passed\n", failures);
    return failures ? 1 : 0;
}
