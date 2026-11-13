// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL codec, robustness: random bits, all zeros, all ones, bit-flipped good frames, wrong-shape frames, a partial last frame and an
// empty stream all decode to the right number of samples without a crash.
#include "dect2/kaal/kaal_codec.h"
#include <cstdio>
#include <random>
using namespace dect2;
#include "../common/test_util.h"
#include "kaal_test_common.h"

int main() {
    KaalCodec c;
    std::mt19937 rng(5);
    {   // random bits, 40 frames
        std::vector<uint8_t> r(40 * 320);
        for (auto& b : r) b = rng() & 1;
        CHECK(c.decode(r).size() == 40u * 320u, "random");
        for (int t = 0; t < 40; t++) r[t * 320] = r[t * 320 + 1] = r[t * 320 + 2] = r[t * 320 + 3] = 0;   // speech type with random content
        CHECK(c.decode(r).size() == 40u * 320u, "random speech");
    }
    CHECK(c.decode(std::vector<uint8_t>(10 * 320, 0)).size() == 3200u, "zeros");
    CHECK(c.decode(std::vector<uint8_t>(10 * 320, 1)).size() == 3200u, "ones");
    {   // good frames with flipped bits and non-binary bytes
        const auto x = kaalTestSignal();
        auto bits = c.encode(x);
        for (int i = 0; i < 2000; i++) bits[rng() % bits.size()] ^= 1;
        CHECK(c.decode(bits).size() == bits.size(), "flipped");
        bits[700] = 7;   // non-binary byte: that frame is lost
        CHECK(c.decode(bits).size() == bits.size(), "non-binary");
    }
    {   // wrong shapes
        std::vector<std::vector<uint8_t>> f = {{}, std::vector<uint8_t>(1, 1), std::vector<uint8_t>(319, 0), std::vector<uint8_t>(321, 1),
                                               std::vector<uint8_t>(320, 0), std::vector<uint8_t>(3200, 1)};
        CHECK(c.decode(f).size() == 6u * 320u, "shapes");
    }
    CHECK(c.decode(std::vector<uint8_t>(320 * 3 + 100, 0)).size() == 4u * 320u, "partial last frame");
    CHECK(c.decode(std::vector<uint8_t>()).empty(), "empty");
    CHECK(c.decode(nullptr, 0).empty(), "null");
    return testSummary("kaal_robust");
}
