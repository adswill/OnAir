// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pager sensitivity: one cycle of the test signal (27 POCSAG and FLEX pages at every speed) at 0 dB with three noise seeds. The soft
// repair of code words and the check of the first batch after a sync bring 76 of 216 pages over eight seeds where the hard decoder got
// 48, with fewer false pages. Here: right pages (address, type and text as sent, not damaged), pages that match nothing, and noise alone.
#include "dect2/pager/pager_gen.h"
#include "dect2/pager/pager_rx.h"
#include "dect2/pager/pager_tel.h"
#include "../common/test_util.h"
#include "../common/voice_sens.h"
using namespace dect2;

static void score(const PagerTelemetry& t, int& right, int& wrong) {
    const auto want = pagerTestMessages();
    if (!t.messages) return;
    for (const auto& m : *t.messages) {
        bool addr = false, exact = false;
        for (const auto& w : want)
            if (w.address == m.address && w.flex == m.flex && w.speed == m.speed) { addr = true; if (!m.damaged && (w.text == m.text || w.type == kPagerTone)) exact = true; }
        if (exact) right++; else if (!addr) wrong++;
    }
}

int main() {
    const double off = pagerTuning().tuneOffsetHz, secs = pagerCycleSeconds() + 0.5;
    int r30 = 0, w30 = 0, r0 = 0, w0 = 0, rn = 0, wn = 0;
    score(voicesens::run<PagerReceiver, PagerTelemetry>(25, 1.2e6, off, secs, 30, 0, 1), r30, w30);
    for (int seed = 1; seed <= 3; seed++) score(voicesens::run<PagerReceiver, PagerTelemetry>(25, 1.2e6, off, secs, 0, 0, seed), r0, w0);
    score(voicesens::run<PagerReceiver, PagerTelemetry>(25, 1.2e6, off, 60, 30, 0, 1, true), rn, wn);
    printf("pager sens: 30 dB %d right %d wrong; 0 dB (3 seeds) %d right %d wrong; noise %d pages\n", r30, w30, r0, w0, rn + wn);
    CHECK(r30 == 27 && w30 == 0, "30 dB: %d right, %d wrong", r30, w30);
    CHECK(r0 >= 25, "0 dB: %d right of 81", r0);
    CHECK(w0 <= 2, "0 dB: %d wrong", w0);
    CHECK(rn + wn == 0, "noise: %d pages", rn + wn);
    return testSummary("pager sens");
}
