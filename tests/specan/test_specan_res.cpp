// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Spectrum analyser resolution: the bin width that is applied for the one asked for (dect2/specan/specan_res.h).
#include "dect2/specan/specan_res.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"
int main() {
    // the HackRF sweep: 20 Msps blocks of at most 4096 points, so 4.88 kHz is the finest bin; 2.8 kHz cannot be had
    const SaRes a = saPickRes(2.8e3, 0, true);
    CHECK(a.nfft == 4096 && std::fabs(a.binHz - 20e6 / 4096) < 1e-6, "2.8 kHz native -> %d points, %.1f Hz", a.nfft, a.binHz);
    CHECK(std::fabs(saPickRes(5e3, 0, true).binHz - 20e6 / 4096) < 1e-6, "5 kHz native");
    CHECK(std::fabs(saPickRes(10e3, 0, true).binHz - 20e6 / 2048) < 1e-6, "10 kHz native");
    CHECK(std::fabs(saPickRes(1e6, 0, true).binHz - 20e6 / 32) < 1e-6 || saPickRes(1e6, 0, true).binHz <= 1e6 * 1.01, "1 MHz native stays at or under 1 MHz");
    // a stepped sweep or a file can go finer: 8 Msps, 2.8 kHz asked -> 4096 points (1.95 kHz), never wider than asked
    const SaRes b = saPickRes(2.8e3, 8e6, false);
    CHECK(b.nfft == 4096 && b.binHz <= 2.8e3 * 1.01, "2.8 kHz at 8 Msps -> %d points, %.1f Hz", b.nfft, b.binHz);
    // every offered choice maps to itself (what the box shows is what is applied), native and stepped
    for (bool nat : {true, false}) {
        const auto ch = saBinChoices(nat ? 0 : 2.4e6, nat);
        CHECK(!ch.empty(), "choices exist");
        for (size_t i = 0; i < ch.size(); i++) {
            const SaRes r = saPickRes(ch[i], nat ? 0 : 2.4e6, nat);
            CHECK(std::fabs(r.binHz - ch[i]) < 1e-6 * ch[i], "choice %zu: %.2f -> %.2f", i, ch[i], r.binHz);
            if (i) CHECK(std::fabs(ch[i - 1] / ch[i] - 2) < 1e-9, "powers of two");
        }
    }
    CHECK(saBinChoices(0, false).empty() && saPickRes(0, 1e6, false).binHz == 0, "no rate, no choices");
    printf(fails ? "FAILED\n" : "specan res ok\n");
    return fails ? 1 : 0;
}
