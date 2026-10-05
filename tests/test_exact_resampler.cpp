// The streaming resampler: a tone keeps its frequency and phase through any ratio, and block boundaries do not matter.
#include "dect2/exact_resampler.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    struct R { double in, out; } rates[] = {{10e6, 512e6 / 63}, {8e6, 512e6 / 63}, {20e6, 512e6 / 63}, {6.5e6, 512e6 / 63}, {10e6, 64e6 / 7}, {2.048e6, 8.126984e6}};
    for (auto& r : rates) {
        ExactResampler a, b;
        CHECK(a.configure(r.in, r.out) && b.configure(r.in, r.out), "configure %.3f -> %.3f", r.in, r.out);
        const double f = 0.37e6;   // a tone well inside the band
        const size_t n = 200000;
        std::vector<cf32> x(n), whole, chunks;
        for (size_t i = 0; i < n; i++) { const double ph = 2 * M_PI * f * (double)i / r.in; x[i] = cf32((float)std::cos(ph), (float)std::sin(ph)); }
        a.process(x.data(), n, whole);
        std::mt19937 rng(3);
        for (size_t i = 0; i < n;) { size_t k = 1 + rng() % 5000; if (i + k > n) k = n - i; b.process(x.data() + i, k, chunks); i += k; }
        CHECK(whole.size() == chunks.size(), "block independence: %zu vs %zu samples", whole.size(), chunks.size());
        double maxDiff = 0;
        for (size_t i = 0; i < std::min(whole.size(), chunks.size()); i++) maxDiff = std::max(maxDiff, (double)std::abs(whole[i] - chunks[i]));
        CHECK(maxDiff < 2e-3, "blocks give the same samples (diff %g)", maxDiff);
        // the tone: after the start-up its phase advances by 2 pi f / out per output sample, and the amplitude stays 1
        double maxPhaseErr = 0, minAmp = 9, maxAmp = 0;
        for (size_t i = 200; i + 200 < whole.size(); i++) {
            // sample i of the output is at input position i * in / out (the first output sits on input sample 0)
            const double t = (double)i * r.in / r.out;
            const double ph = 2 * M_PI * f * t / r.in;
            const cf32 want((float)std::cos(ph), (float)std::sin(ph));
            maxPhaseErr = std::max(maxPhaseErr, (double)std::abs(std::arg(whole[i] * std::conj(want))));
            minAmp = std::min(minAmp, (double)std::abs(whole[i])); maxAmp = std::max(maxAmp, (double)std::abs(whole[i]));
        }
        CHECK(maxPhaseErr < 2e-3, "%.3f -> %.3f: phase error %g rad", r.in, r.out, maxPhaseErr);
        CHECK(minAmp > 0.995 && maxAmp < 1.005, "%.3f -> %.3f: amplitude %g .. %g", r.in, r.out, minAmp, maxAmp);
        const double expectN = (double)n * r.out / r.in;
        CHECK(std::fabs((double)whole.size() - expectN) < 25 * std::max(1.0, r.out / r.in), "%.3f -> %.3f: %zu samples (want about %.1f)", r.in, r.out, whole.size(), expectN);
    }
    printf(fails ? "exact resampler: FAILED\n" : "exact resampler: ok\n");
    return fails ? 1 : 0;
}
