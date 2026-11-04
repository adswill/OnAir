// GPU FFT / power accumulate / FIR decimator (GpuFft, Metal) against the CPU code (fftSplit, a plain FIR loop): the largest relative error, and the
// time of each per call. A numeric tolerance (float rounding), not bit-exact. Passes without a GPU. The table goes into docs/GPU-AUDIT-3.7.md.
#include "dect2/dsp/dsp_compat.h"
#include "dect2/dsp/gpu_fft.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace dect2;
using Clk = std::chrono::steady_clock;
static double ms(Clk::time_point a, Clk::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

int main() {
    if (!GpuFft::instance().available()) { printf("gpu fft: no GPU backend, nothing to compare\n"); return 0; }
    printf("device: %s\n", GpuFft::instance().deviceName());
    std::mt19937 rng(9);
    std::normal_distribution<float> g(0.f, 1.f);
    int fails = 0;
    for (size_t n : {1024u, 4096u, 8192u, 32768u, 65536u}) {
        for (int nb : {1, 2, 8}) {
            std::vector<cf32> x((size_t)nb * n);
            for (auto& v : x) v = cf32(g(rng), g(rng));
            std::vector<float> win(n); hannWindowNorm(win.data(), (int)n);
            const int log2n = (int)std::lround(std::log2((double)n));
            std::vector<float> ref(n, 0.f), re(n), im(n);
            const int reps = 20;
            auto t0 = Clk::now();
            for (int r = 0; r < reps; r++) {
                std::fill(ref.begin(), ref.end(), 0.f);
                for (int b = 0; b < nb; b++) {
                    for (size_t i = 0; i < n; i++) { re[i] = x[(size_t)b * n + i].real() * win[i]; im[i] = x[(size_t)b * n + i].imag() * win[i]; }
                    fftSplit(re.data(), im.data(), log2n, false);
                    for (size_t i = 0; i < n; i++) ref[i] += re[i] * re[i] + im[i] * im[i];
                }
            }
            const double tCpu = ms(t0, Clk::now()) / reps;
            std::vector<float> acc(n, 0.f);
            GpuFft::instance().powerAccumulate(x.data(), nb, n, win.data(), acc.data());   // warm-up
            t0 = Clk::now();
            for (int r = 0; r < reps; r++) { std::fill(acc.begin(), acc.end(), 0.f); GpuFft::instance().powerAccumulate(x.data(), nb, n, win.data(), acc.data()); }
            const double tGpu = ms(t0, Clk::now()) / reps;
            double maxErr = 0, peak = 0;
            for (size_t i = 0; i < n; i++) peak = std::max(peak, (double)ref[i]);
            for (size_t i = 0; i < n; i++) maxErr = std::max(maxErr, std::fabs((double)acc[i] - ref[i]) / peak);
            printf("power spectrum n=%6zu blocks=%d: CPU %.3f ms, GPU %.3f ms per call, max error %.2e of the peak bin\n", n, nb, tCpu, tGpu, maxErr);
            if (maxErr > 1e-4) { printf("FAIL: error too large\n"); fails++; }
        }
    }
    {   // inverse round trip
        const size_t n = 16384;
        std::vector<cf32> x(n), y;
        for (auto& v : x) v = cf32(g(rng), g(rng));
        y = x;
        if (!GpuFft::instance().transform(y.data(), 1, n, false) || !GpuFft::instance().transform(y.data(), 1, n, true)) { printf("FAIL: transform\n"); fails++; }
        double e = 0;
        for (size_t i = 0; i < n; i++) e = std::max(e, (double)std::abs(y[i] / (float)n - x[i]));
        printf("inverse round trip n=%zu: max error %.2e\n", n, e);
        if (e > 1e-4) { printf("FAIL: round trip\n"); fails++; }
    }
    for (int dec : {8, 50}) {   // FIR decimator, 2.4 Msps block of 100 ms
        const int nTaps = dec * 8;
        const size_t nOut = 240000 / (size_t)dec * 4;
        std::vector<float> taps(nTaps);
        for (int t = 0; t < nTaps; t++) taps[t] = (float)(std::sin(0.3 * (t - nTaps / 2.0 + 0.5)) / (0.3 * (t - nTaps / 2.0 + 0.5)) / nTaps);
        std::vector<cf32> x((nOut - 1) * dec + nTaps), o(nOut), r(nOut);
        for (auto& v : x) v = cf32(g(rng), g(rng));
        auto t0 = Clk::now();
        for (size_t i = 0; i < nOut; i++) { float a = 0, b = 0; const cf32* p = x.data() + i * dec; for (int t = 0; t < nTaps; t++) { a += taps[t] * p[t].real(); b += taps[t] * p[t].imag(); } r[i] = cf32(a, b); }
        const double tCpu = ms(t0, Clk::now());
        GpuFft::instance().firDecimate(x.data(), nOut, taps.data(), nTaps, dec, o.data());
        t0 = Clk::now();
        for (int k = 0; k < 5; k++) GpuFft::instance().firDecimate(x.data(), nOut, taps.data(), nTaps, dec, o.data());
        const double tGpu = ms(t0, Clk::now()) / 5;
        double e = 0;
        for (size_t i = 0; i < nOut; i++) e = std::max(e, (double)std::abs(o[i] - r[i]));
        printf("FIR decimate by %d (%d taps, %zu outputs): CPU (scalar loop) %.2f ms, GPU %.2f ms, max error %.2e\n", dec, nTaps, nOut, tCpu, tGpu, e);
        if (e > 1e-3) { printf("FAIL: FIR error\n"); fails++; }
    }
    printf(fails ? "gpu fft: %d FAILED\n" : "gpu fft: ok\n", fails);
    return fails ? 1 : 0;
}
