// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

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
    // 4.1 sensitivity: SNR in the 25 kHz channel (the generator's definition), 2 Msps, carrier offsets of 0 and +-2.5 kHz, a +50 ppm sample
    // clock on the offset runs, two noise seeds. Before 4.1 the squelch worked on the power of the whole input band and needed about 21 dB here.
    // Then noise alone (10 s, two levels) must give no frame at all.
    {
        auto run = [](double snr, double cfo, int seed, double secs) {
            SynthConfig sc;
            sc.snrDb = snr;
            sc.cfoHz = cfo;
            sc.mode = 84;
            sc.modeOpt[1] = seed;
            const double rate = 2e6, clk = cfo != 0 ? 1.0 + 50e-6 : 1.0;
            auto syn = makeVdl4Synth(sc, rate * clk);
            Vdl4Receiver rx;
            rx.configure(rate);
            rx.setCenterHz(136.85e6);
            rx.setSignalOffset(0);
            std::vector<cf32> buf(65536);
            const size_t total = (size_t)(rate * secs);
            for (size_t done = 0; done < total;) {
                const size_t n = std::min(buf.size(), total - done);
                syn->generate(buf.data(), n);
                rx.feed(buf.data(), n);
                done += n;
            }
            Vdl4Telemetry t;
            rx.telemetry(t, ~0ull);
            return t;
        };
        const double snrs[] = {10, 12, 14, 16, 18, 20, 22, 24};
        for (double cfo : {0.0, 2500.0, -2500.0}) {
            double thr = 99;
            for (int k = 7; k >= 0; k--) {
                int ok = 0;
                for (int seed = 1; seed <= 2; seed++) ok += run(snrs[k], cfo, seed, 1.8).blocksOk >= 2 ? 1 : 0;
                printf("  sens cfo %+5.0f snr %4.1f dB: %d of 2 trials with 2+ of 4 bursts\n", cfo, snrs[k], ok);
                if (ok == 2) thr = snrs[k]; else break;
            }
            printf("  vdl4 threshold (25 kHz SNR), cfo %+.0f: %.0f dB\n", cfo, thr);
            CHECK(thr <= 16, "cfo %+.0f: needs %.0f dB in 25 kHz (4.1 limit 16)", cfo, thr);
        }
    }
    {
        // noise alone: the bursts 40 dB under the noise in their 25 kHz channel stand in for no signal
        uint64_t bad = 0, good = 0;
        for (int seed = 1; seed <= 2; seed++) {
            SynthConfig sc;
            sc.snrDb = -40;
            sc.mode = 84;
            sc.modeOpt[1] = seed;
            auto syn = makeVdl4Synth(sc, 2e6);
            Vdl4Receiver rx;
            rx.configure(2e6);
            rx.setCenterHz(136.85e6);
            rx.setSignalOffset(0);
            std::vector<cf32> buf(65536);
            const size_t total = (size_t)(2e6 * 10);
            for (size_t done = 0; done < total;) {
                const size_t n = std::min(buf.size(), total - done);
                syn->generate(buf.data(), n);
                rx.feed(buf.data(), n);
                done += n;
            }
            Vdl4Telemetry t;
            rx.telemetry(t, ~0ull);
            good += t.blocksOk; bad += t.blocksBad;
        }
        printf("  noise only (2 x 10 s): %llu frames, %llu failed the FCS\n", (unsigned long long)good, (unsigned long long)bad);
        CHECK(good == 0, "noise alone gave %llu frames", (unsigned long long)good);
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
