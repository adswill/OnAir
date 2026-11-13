// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Inmarsat Aero demodulators: acquisition (time to the first good SU) against Eb/N0 and carrier offset, off-centre carriers, drift.
// Reports the lock time per rate; asserted: lock inside the budget at the rate's sensitivity limit and across the search range.
#include "dect2/aero/aero_demod.h"
#include "dect2/aero/aero_phy.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;

#include "../common/test_util.h"

// seconds until the first frame with a good SU (-1 when none within maxSecs)
static double lockTime(int rate, double fs, double maxSecs, double ebn0Db, double cfo, double drift, uint32_t seed, double searchHz = 700) {
    std::mt19937 g(seed);
    AeroFrameEncoder enc(rate);
    AeroModulator mod(rate, fs, 0);
    AeroDemod dem(rate, fs, searchHz);
    double first = -1, now = 0;
    dem.setCallback([&](AeroFrameEvent& e) { if (first < 0 && e.susOk > 0) first = now; });
    const AeroFrameFormat* f = aeroFrameFormat(rate);
    const double sigma = std::sqrt(fs / (2.0 * rate * std::pow(10.0, ebn0Db / 10)));
    genutil::NoiseSource noise(seed + 1);
    const size_t total = (size_t)(maxSecs * fs), chunk = 2048;
    std::vector<cf32> buf(chunk);
    double ph = 0;
    int fno = 0;
    for (size_t done = 0; done < total && first < 0;) {
        while (mod.queuedBits() < 2u * f->totalBits()) {
            std::vector<uint8_t> info(f->infoBytes());
            for (size_t k = 0; k + 12 <= info.size(); k += 12) { for (int i = 0; i < 10; i++) info[k + i] = (uint8_t)g(); aeroSuSetCrc(&info[k]); }
            const auto bits = enc.frame(info.data(), aeroHeader(1, 0, fno >> 4, fno & 15));
            fno++;
            mod.pushBits(bits.data(), bits.size());
        }
        mod.generate(buf.data(), chunk);
        for (size_t i = 0; i < chunk; i++) {
            const double t = (double)(done + i) / fs;
            buf[i] *= cf32((float)std::cos(ph), (float)std::sin(ph));
            ph += 2 * M_PI * (cfo + drift * t) / fs;
            if (ph > M_PI) ph -= 2 * M_PI;
        }
        noise.add(buf.data(), chunk, (float)sigma);
        done += chunk;
        now = (double)done / fs;
        dem.feed(buf.data(), chunk);
    }
    return first;
}

int main() {
    const int rates[3] = {600, 1200, 10500};
    for (int rate : rates) {
        const double fs = rate == 10500 ? 48000 : 9600;
        const double budget = rate == 600 ? 90 : rate == 1200 ? 45 : 20;
        for (double eb : {8.0, 5.0, 3.0, 2.0, 1.0}) {
            for (double cfo : {0.0, 350.0, -650.0}) {
                int ok = 0; double sum = 0;
                const int N = 4;
                for (int s = 0; s < N; s++) {
                    const double t = lockTime(rate, fs, budget, eb, cfo, 0, 100 + (uint32_t)s * 7 + (uint32_t)(eb * 3));
                    if (t >= 0) { ok++; sum += t; }
                }
                printf("rate %5d Eb/N0 %3.0f dB cfo %5.0f Hz: locked %d/%d, mean lock %.1f s\n", rate, eb, cfo, ok, N, ok ? sum / ok : -1.0);
                if (eb == 1) CHECK(ok >= 3, "rate %d eb 1 cfo %.0f: locked %d of %d (the carrier search integrates up to 32 blocks)", rate, cfo, ok, N);
                if (eb >= 2) CHECK(ok == N, "rate %d eb %.0f cfo %.0f: locked %d of %d", rate, eb, cfo, ok, N);
            }
        }
        // Doppler-like drift (Hz/s): an aircraft at speed shifts the carrier by up to a few Hz/s
        int ok = 0;
        for (int s = 0; s < 3; s++) if (lockTime(rate, fs, budget, 6, 100, 3.0, 500 + (uint32_t)s) >= 0) ok++;
        printf("rate %5d drift 3 Hz/s 6 dB: locked %d/3\n", rate, ok);
        CHECK(ok == 3, "rate %d drift lock %d", rate, ok);
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
