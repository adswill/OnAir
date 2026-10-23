// Whole-span (wide bank) cost: N DMR carriers on a 12.5 kHz raster in one span, fed through WideModes like the engine does.
//
//   bench_wide [--carriers N] [--rate Msps] [--seconds S] [--threads 0|1]
//
// The span is generated once (every carrier its own DmrSignal, shifted and summed; not counted), then fed in 64 ms blocks. Reported: CPU seconds
// (user + system, all threads) per second of signal, the same per carrier, peak RSS, the carriers the bank decoded at the end, and a digest of the
// calls the carriers logged (it must not change with an optimisation). Wall time is never used for the speed.
#include "dect2/dmr/dmr_gen.h"
#include "dect2/engine/wide_modes.h"
#include "dect2/engine/wide_bank.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/resource.h>
using namespace dect2;

static double cpuSec() {
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
    return r.ru_utime.tv_sec + r.ru_stime.tv_sec + 1e-6 * (r.ru_utime.tv_usec + r.ru_stime.tv_usec);
}

int main(int argc, char** argv) {
    int nCar = 50, threads = 1;
    double rate = 2.4e6, secs = 8;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--carriers") && i + 1 < argc) nCar = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atof(argv[++i]) * 1e6;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) secs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
    }
    WideParallel::enabled() = threads != 0;
    const size_t N = (size_t)(rate * secs);
    std::vector<cf32> span(N, cf32(0, 0));
    const double step = 12500;
    const double amp = 1.0 / std::sqrt((double)nCar) * 0.8;
    {
        std::vector<cf32> one(N);
        for (int c = 0; c < nCar; c++) {
            DmrGenConfig g; g.rate = rate; g.seed = 100 + c; g.cc = 1 + c % 14; g.snrDb = 30; g.rms = 0.22; g.traffic = 2;
            DmrSignal s(g);
            s.generate(one.data(), N);
            int k = c - nCar / 2; if (k >= 0) k++;   // symmetric on the raster, centre cell skipped
            const double w = 2 * M_PI * k * step / rate;
            const std::complex<double> rot(std::cos(w), std::sin(w));
            std::complex<double> p(1, 0);
            for (size_t i = 0; i < N; i++) {
                const float cr = (float)p.real(), sr = (float)p.imag();
                span[i] += cf32(one[i].real() * cr - one[i].imag() * sr, one[i].real() * sr + one[i].imag() * cr) * (float)amp;
                p = cf32(p.real() * rot.real() - p.imag() * rot.imag(), p.real() * rot.imag() + p.imag() * rot.real()) * 1.0f;
                if ((i & 4095) == 4095) p /= std::abs(p);
            }
        }
    }
    WideModes wm;
    WideTuning t = wm.tuning();
    t.maxCarriers = 128; t.cpuBudget = 0;   // no cap: the cost per carrier is what is measured
    wm.setTuning(t);
    std::vector<std::string> lines;
    wm.setLogCallback([&](const std::string& l) { lines.push_back(l); });
    wm.configure(10, rate, 0);
    const size_t blk = (size_t)(rate * 0.064);
    const size_t warm = (size_t)(rate * 2.0) / blk * blk;
    size_t i = 0;
    for (; i < warm && i + blk <= N; i += blk) wm.feed(10, span.data() + i, blk);
    const double c0 = cpuSec();
    const size_t startI = i;
    for (; i + blk <= N; i += blk) wm.feed(10, span.data() + i, blk);
    const double c1 = cpuSec();
    const double sig = (double)(i - startI) / rate;
    RxTelemetry tel; uint64_t seq = 0;
    wm.telemetry(10, tel, seq);
    uint64_t h = 1469598103934665603ull;
    for (auto& l : lines) for (unsigned char ch : l) { h ^= ch; h *= 1099511628211ull; }
    rusage r{}; getrusage(RUSAGE_SELF, &r);
    const double cost = (c1 - c0) / sig;
    printf("carriers=%d rate=%.2fM threads=%d live=%zu cost=%.4f cpu_s_per_s per_carrier=%.5f rss=%.0fMB lines=%zu digest=%016llx\n", nCar, rate / 1e6, threads,
           wm.liveCount(10), cost, cost / nCar, r.ru_maxrss / 1048576.0, lines.size(), (unsigned long long)h);
    return 0;
}
