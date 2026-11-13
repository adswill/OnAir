// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A repeated SDC is added to the earlier copies when its CRC fails. Simulated (drm_gen), white noise: the SDC blocks that come through with and without. The transmitter's SDC here repeats unchanged.
#include "../data/drm/testkit.h"
#include "dect2/drm/drm_dec.h"
#include "dect2/drm/drm_gen.h"
#include <cstdio>
using namespace drmtest;
using namespace dect2;
#include "../common/test_util.h"
namespace dect2 { namespace drm { extern std::atomic<bool> gDrmSdcCombine; } }

static std::pair<uint64_t, uint64_t> run(double snr, bool combine, int mode) {
    drm::gDrmSdcCombine = combine;
    SynthConfig sc; sc.snrDb = snr;
    DrmTxConfig tc; tc.mode = mode; tc.occupancy = 3; tc.mscQam = 16; tc.sdcRotate = false; tc.sendTime = false; tc.textMessage = false;
    std::shared_ptr<ModeSynth> syn = makeDrmSynth(tc, sc, 96000);
    Gen g = [syn](cf32* out, size_t n) { syn->generate(out, n); };
    RunOpt o; o.secs = 40; o.text = false;
    Result r = runRx(g, 96000, o);
    return {r.tel.sdcOk, r.tel.sdcBad};
}

int main() {
    int gain = 0;
    for (int mode : {1, 2}) for (double snr : {8.0, 7.0, 6.0}) {
        auto off = run(snr, false, mode), on = run(snr, true, mode);
        printf("mode %d SNR %.0f dB: SDC ok %llu bad %llu without, ok %llu bad %llu with\n", mode, snr, (unsigned long long)off.first, (unsigned long long)off.second, (unsigned long long)on.first, (unsigned long long)on.second);
        CHECK(on.first >= off.first, "never worse");
        if (on.first > off.first) gain++;
    }
    CHECK(gain >= 1, "combining helped somewhere in the sweep");
    return testSummary("drm_sdccomb");
}
