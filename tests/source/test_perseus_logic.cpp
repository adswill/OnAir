// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Perseus sample conversion (24-bit signed little-endian I/Q to float) and the attenuator steps, without a library or a radio.
#include "../../core/src/source/perseus_logic.h"
#include <cstdio>

using namespace dect2::native;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main() {
    // I = 0x000001 (+1), Q = 0xFFFFFF (-1); I = 0x7FFFFF (max), Q = 0x800000 (min); I = 0x400000 (+0.5), Q = 0xC00000 (-0.5); a half sample left over
    const uint8_t in[] = {0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
                          0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x80,
                          0x00, 0x00, 0x40, 0x00, 0x00, 0xC0,
                          0x12, 0x34, 0x56};
    float out[8] = {0};
    const size_t n = perseusConvert(in, sizeof in, out);
    CHECK(n == 3, "sample count %zu", n);
    const float lsb = 1.0f / 8388608.0f;
    CHECK(out[0] == lsb, "I +1: %g", out[0]);
    CHECK(out[1] == -lsb, "Q -1: %g", out[1]);
    CHECK(out[2] == 8388607.0f / 8388608.0f, "I max: %g", out[2]);
    CHECK(out[3] == -1.0f, "Q min: %g", out[3]);
    CHECK(out[4] == 0.5f, "I +0.5: %g", out[4]);
    CHECK(out[5] == -0.5f, "Q -0.5: %g", out[5]);
    CHECK(out[6] == 0.0f, "nothing written past the last whole sample");
    CHECK(perseusConvert(in, 5, out) == 0, "less than one sample");
    CHECK(perseusAttStep(30) == 0 && perseusAttStep(100) == 0, "top of the slider is 0 dB");
    CHECK(perseusAttStep(20) == 1 && perseusAttStep(10) == 2 && perseusAttStep(0) == 3 && perseusAttStep(-5) == 3, "steps");
    CHECK(perseusAttStep(24) == 1 && perseusAttStep(14) == 2, "rounding");
    printf(fails ? "perseus_logic: %d FAILED\n" : "perseus_logic: all passed\n", fails);
    return fails ? 1 : 0;
}
