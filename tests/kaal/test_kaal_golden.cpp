// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL codec, golden bits: the encoder on a fixed 1 s two-tone signal gives the bits of the Python reference for the first frames;
// every frame is exactly 320 bits of 0 or 1 and the count is ceil(n / 320) + 1.
#include "dect2/kaal/kaal_codec.h"
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"
#include "kaal_test_common.h"

int main() {
    KaalCodec c;
    const auto x = kaalTestSignal();
    const auto bits = c.encode(x);
    CHECK(bits.size() == 51u * 320u, "frames %zu", bits.size() / 320);
    bool bin = true;
    for (uint8_t b : bits) if (b > 1) bin = false;
    CHECK(bin, "bits are 0 or 1");
    for (int f = 0; f < 4; f++) {
        const auto g = kaalGoldBits(f);
        int diff = 0;
        for (int i = 0; i < 320; i++) diff += bits[f * 320 + i] != g[i];
        CHECK(diff == 0, "frame %d differs from the Python in %d bits", f, diff);
    }
    int later = 0;   // frames 4..9 are expected to match too; counted, not fatal beyond the first 4
    for (int f = 4; f < kGoldFrames; f++) {
        const auto g = kaalGoldBits(f);
        for (int i = 0; i < 320; i++) later += bits[f * 320 + i] != g[i];
    }
    printf("frames 4..9: %d differing bits\n", later);
    // lengths: 0 samples, 1 sample, exactly one frame, one frame + 1
    CHECK(c.encode(x.data(), 0).size() == 320u, "n=0");
    CHECK(c.encode(x.data(), 1).size() == 640u, "n=1");
    CHECK(c.encode(x.data(), 320).size() == 640u, "n=320");
    CHECK(c.encode(x.data(), 321).size() == 960u, "n=321");
    // loud full-scale input still gives 320-bit frames
    std::vector<int16_t> loud(3200);
    for (size_t i = 0; i < loud.size(); i++) loud[i] = (i / 3) % 2 ? 32767 : -32768;
    CHECK(c.encode(loud).size() == 11u * 320u, "loud");
    return testSummary("kaal_golden");
}
