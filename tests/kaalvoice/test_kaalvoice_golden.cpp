// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL voice codec 4 is deterministic: the bit stream and the decoded samples of fixed inputs are the same on every platform (the codec
// uses its own elementary functions and is built without fused multiply-add contraction). The digest covers several profiles, both
// frame lengths, both sample rates, DTX with the in-band type bit, the noise suppressor and a lossy channel.
// A different digest means the codec output changed: if that was intended (a codec change), the stream is a new bit-stream version:
// update docs/KAAL-CODEC.md and record the new values with ONAIR_GOLDEN_DUMP=1. The receiver-side enhancer is off here (its output
// is covered by test_kaalvoice_enhance), so this digest also shows that the plain decoder did not change.
#include "kv_test_common.h"
#include "../common/golden.h"
#include <cstdio>
#include "../common/test_util.h"

using namespace kvtest;

static constexpr uint64_t kGoldenAll = 0x66628670c66ef620ull, kGoldenPortable = 0x66628670c66ef620ull;

int main() {
    golden::Dual d;
    struct Case { Profile p; int ms, fs; bool dtx, ns, ib; double noiseDb; };
    const Case cs[] = {
        {Profile::K6_0, 40, 16000, false, false, false, -200}, {Profile::K4_8, 20, 16000, false, false, false, -200},
        {Profile::HQ16, 20, 16000, false, false, false, -200}, {Profile::K3_2, 40, 16000, false, false, false, -200},
        {Profile::K8_0, 40, 8000, true, true, true, -20},      {Profile::HQ12, 20, 16000, true, false, false, -30},
    };
    bool repeat = true;
    for (const Case& k : cs) {
        Config c = cfg(k.p, k.ms, k.fs, k.dtx);
        c.noiseSuppress = k.ns;
        c.inBandType = k.ib;
        c.enhance = 0;   // the plain decoder: the enhancer has its own digest (test_kaalvoice_enhance)
        const std::vector<int16_t> x = eval::synthSpeech(k.fs, 2, 1.5, 3, k.noiseDb);
        auto channel = [](ChannelFrame& f) { f.lost = f.index % 13 == 7; };
        const Run r = run(c, x, channel);
        const Run r2 = run(c, x, channel);
        repeat &= r.out == r2.out && r.frames == r2.frames;
        golden::Digest one;
        for (size_t i = 0; i < r.frames.size(); i++) {
            d.add((int)r.types[i]);
            d.add(r.frames[i]);
            one.add(r.frames[i]);
        }
        d.add(r.out);
        one.add(r.out);
        printf("  %s %d ms %d Hz dtx %d ns %d in-band %d: %016llx\n", profileName(k.p), k.ms, k.fs, k.dtx, k.ns, k.ib, (unsigned long long)one.value());
    }
    CHECK(repeat, "two runs in one process give the same output");
    fails += golden::checkDual("kaalvoice", d, kGoldenAll, kGoldenPortable, true);
    return testSummary("kaalvoice_golden");
}
