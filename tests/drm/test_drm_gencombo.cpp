// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The DRM test signal for every choice of the generator panel (mode, occupancy, QAM, protection): the synthetic source must always produce samples.
// Regression (3.9, Discord report): 16-QAM with protection 2 or 3 in modes A to D (which have levels 0 and 1 only) made an invalid transmitter, its
// fallback kept the invalid protection, and DrmSynth::generate() looped forever waiting for samples, freezing the app. Now the protection is clamped
// to the levels the QAM has, the fallback is the usual signal, and an empty transmitter gives silence. Simulated signals only.
#include "../data/drm/testkit.h"
#include "dect2/drm/drm_gen.h"
#include "dect2/drm/drm_defs.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace drmtest;
using namespace dect2;
#include "../common/test_util.h"

int main() {
    // a hang is the bug: end the program with a failure instead of waiting for the ctest timeout
    std::atomic<bool> done{false};
    std::thread dog([&] {
        for (int i = 0; i < 600 && !done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!done) { printf("FAIL: the generator hung (no samples after 300 s)\n"); fflush(stdout); std::_Exit(1); }
    });

    // 1. the configuration: the protection level always fits the QAM
    for (int m = 0; m <= 5; m++)
        for (int q = 0; q <= 1; q++)
            for (int p = 0; p <= 4; p++) {
                SynthConfig sc; sc.mode = 12;
                sc.modeOpt[0] = m; sc.modeOpt[2] = q; sc.modeOpt[3] = p;
                const DrmTxConfig tc = drmTxConfigFromSynth(sc);
                const int qb = tc.mscQam == 64 ? 6 : tc.mscQam == 16 ? 4 : 2;
                CHECK(tc.protB >= 0 && tc.protB < drm::mscProtLevels(tc.mode, qb), "mode opt %d QAM opt %d prot opt %d: protection %d for %d-QAM", m, q, p, tc.protB, tc.mscQam);
            }

    // 2. every combination of the panel makes samples, and a valid transmitter
    int combos = 0, silent = 0;
    for (int m = 1; m <= 5; m++)
        for (int oc = 0; oc <= (m == 5 ? 0 : 6); oc++)
            for (int q = 0; q <= 1; q++)
                for (int p = 0; p <= 4; p++) {
                    SynthConfig sc; sc.mode = 12; sc.snrDb = 30;
                    sc.modeOpt[0] = m; sc.modeOpt[1] = oc; sc.modeOpt[2] = q; sc.modeOpt[3] = p; sc.modeOpt[5] = 4;   // pattern audio: quick
                    const double rate = m == 5 ? 250000 : 48000;
                    auto syn = makeModeSynth(12, sc, rate);
                    CHECK(syn != nullptr, "mode %d occ %d q %d p %d: no synth", m, oc, q, p);
                    if (!syn) continue;
                    std::vector<cf32> buf((size_t)(rate * 0.5));
                    syn->generate(buf.data(), buf.size());
                    double e = 0;
                    for (const cf32& v : buf) e += std::norm(v);
                    if (e < 1e-6 * (double)buf.size()) silent++;
                    combos++;
                }
    printf("  %d combinations generated, %d silent\n", combos, silent);
    CHECK(silent == 0, "%d combinations gave silence (no valid transmitter)", silent);

    // 3. the reported signals decode: mode B and mode A at 16-QAM with "protection 2" and "protection 3" picked (level 1 is sent), 3 kHz off, 25 dB
    for (int mode : {2, 1})
        for (int p : {3, 4}) {
            Sig s; s.mode = mode; s.qam16 = 1; s.prot = p; s.snrDb = 25; s.cfoHz = 3000;
            RunOpt o; o.secs = 14;
            const Result r = runRx(synthGen(s, 192000), 192000, o);
            printf("  mode opt %d 16-QAM prot opt %d: state %d, %d-QAM PL %d, %llu exact %llu bad\n", mode, p, r.tel.state, r.tel.mscQam, r.tel.protB,
                   (unsigned long long)r.exact, (unsigned long long)r.bad);
            CHECK(r.tel.state == 2 && r.tel.mscQam == 16 && r.tel.protB == 1, "mode opt %d prot opt %d: state %d %d-QAM PL %d", mode, p, r.tel.state, r.tel.mscQam, r.tel.protB);
            CHECK(r.exact >= 10 && r.bad <= 1, "mode opt %d prot opt %d: %llu exact %llu bad", mode, p, (unsigned long long)r.exact, (unsigned long long)r.bad);
        }
    done = true;
    dog.join();
    return testSummary("drm_gencombo");
}
