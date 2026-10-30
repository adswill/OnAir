// VDL Mode 4 receiver on the synthetic signal at every common SDR sample rate, with carrier offsets:
// no crash (run with ASan/UBSan too), no spin (CPU-time guard), and the frames decode where the channels are in the band.
// Regression for the RSP1B start-up crash at 2 and 4 Msps (burst trimming erased past the end of the line buffer, and the
// sync search rescanned 8000 bits per sample).
#include "dect2/vdl4/vdl4_gen.h"
#include "dect2/vdl4/vdl4_rx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>
#include "../common/cpu_report.h"
using namespace dect2;
static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)

#if defined(__SANITIZE_ADDRESS__) || defined(__has_feature)
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
#define SANITIZED 1   // the CPU guard only means something at full speed
#endif
#endif
#ifndef SANITIZED
#define SANITIZED 0
#endif

int main() {
    const double rates[] = {0.1e6, 0.25e6, 0.5e6, 1e6, 2e6, 2.4e6, 3e6, 4e6, 5e6, 6e6, 8e6, 10e6, 20e6};
    const double cfos[] = {0, 2500, -2500};
    const double simSec = 1.8;
    for (double rate : rates) {
        for (double cfo : cfos) {
            if (cfo != 0 && rate > 6e6) continue;   // keep the sweep short
            SynthConfig sc;
            sc.snrDb = 30;
            sc.cfoHz = cfo;
            sc.mode = 84;
            auto syn = makeVdl4Synth(sc, rate);
            Vdl4Receiver rx;
            rx.configure(rate);
            rx.setCenterHz(136.85e6);
            rx.setSignalOffset(0);
            std::vector<cf32> buf(65536);
            double cpu = 0;
            size_t total = (size_t)(rate * simSec);
            for (size_t done = 0; done < total;) {
                const size_t n = std::min(buf.size(), total - done);
                syn->generate(buf.data(), n);
                const auto t0 = std::chrono::steady_clock::now();
                rx.feed(buf.data(), n);
                cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                done += n;
            }
            Vdl4Telemetry t;
            rx.telemetry(t, ~0ull);
            // CPU guard: a spin shows as many times real time; allow a slow CI machine and a debug/sanitizer build
            cpuCal("vdl4_rx", cpuCalMsps(rate) + ", cfo " + std::to_string((long long)cfo) + " Hz", cpu / simSec, "cpu-s/s", '<', 6, !SANITIZED);
            CHECK(SANITIZED || cpu < simSec * 6, "%.2f Msps cfo %.0f: %.2f s CPU for %.1f s of samples", rate / 1e6, cfo, cpu, simSec);
            if (rate >= 400e3 - 1) CHECK(t.rateOk, "%.2f Msps should be accepted", rate / 1e6);
            else CHECK(!t.rateOk && vdl4Summary(t).find("too low") != std::string::npos, "%.2f Msps must say the rate is too low: %s", rate / 1e6, vdl4Summary(t).c_str());
            if (rate >= 1e6) CHECK(t.blocksOk >= 2, "%.2f Msps cfo %.0f: %llu frames decoded (%llu bad)", rate / 1e6, cfo, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
            printf("  %5.2f Msps cfo %+5.0f: %llu ok %llu bad, %.2f s CPU\n", rate / 1e6, cfo, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, cpu);
        }
    }
    // zero / absurd rates and a null-length feed must not crash or divide by zero
    for (double r : {0.0, -5.0, 1.0, 1e-3, 1e12, std::nan("")}) {
        Vdl4Receiver rx;
        rx.configure(r);
        std::vector<cf32> z(4096, cf32(0, 0));
        rx.feed(z.data(), z.size());
        rx.feed(z.data(), 0);
        Vdl4Telemetry t;
        rx.telemetry(t, ~0ull);
    }
    if (fails) printf("vdl4 rx: %d FAILED\n", fails);
    else printf("vdl4 rx: all passed\n");
    return fails ? 1 : 0;
}
