// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pager finder (issue #269), simulated signals: one 2 Msps capture with three pager carriers (POCSAG 512, POCSAG 2400 and FLEX 3200/4, the last one
// starting late) and an unmodulated carrier. The scan must list the three pagers at their absolute frequencies with the right speed and a page count,
// the bare carrier (if listed at all) with no speed; the scan stops by itself at its time limit and starts over after configure().
#include "dect2/engine/modes.h"
#include "dect2/pager/pager_find.h"
#include "dect2/pager/pager_gen.h"
#include "dect2/pager/pager_rx.h"
#include <cmath>
#include <cstdio>
#include <memory>
using namespace dect2;
#include "../common/test_util.h"

static const double kFs = 2.0e6, kCentre = 466.0e6, kTol = 4000;

int main() {
    const double off = -pagerTuning().tuneOffsetHz;
    struct Car { int speed; double hz, amp, startSec; };
    const Car car[3] = {{kPocsag512, off - 100000, 1.0, 0}, {kPocsag2400, off + 50000 - 1200, 0.5, 0}, {kFlex3200_4, off + 175000 + 3000, 0.8, 4.0}};
    const double tonHz = off - 350000;
    std::unique_ptr<ModeSynth> g[3];
    double secs = 0;
    for (int k = 0; k < 3; k++) {
        SynthConfig c; c.snrDb = 30; c.cfoHz = car[k].hz - off; c.modeOpt[2] = car[k].speed + 1; c.modeOpt[3] = 21 + k;
        g[k] = makePagerSynth(c, kFs);
        secs = std::max(secs, pagerCycleSeconds(0, car[k].speed + 1) * 2.3 + car[k].startSec);
    }
    PagerFinder pf;
    CHECK(!pf.configure(100e3, kCentre), "a 100 kHz capture is too narrow for a scan");
    CHECK(pf.configure(kFs, kCentre, secs), "configure");
    const size_t blk = (size_t)(kFs * 0.1);
    std::vector<cf32> sum(blk), tmp(blk);
    double ph = 0;
    for (double s = 0; s < secs + 1.0; s += 0.1) {
        std::fill(sum.begin(), sum.end(), cf32(0, 0));
        for (int k = 0; k < 3; k++) {
            g[k]->generate(tmp.data(), blk);
            if (s < car[k].startSec) continue;
            for (size_t i = 0; i < blk; i++) sum[i] += tmp[i] * (float)car[k].amp;
        }
        for (size_t i = 0; i < blk; i++) { ph += 2 * M_PI * tonHz / kFs; sum[i] += 0.3f * cf32((float)std::cos(ph), (float)std::sin(ph)); }
        pf.feed(sum.data(), blk);
    }
    const auto rows = pf.rows();
    printf("%zu candidates, %.1f s scanned\n", rows.size(), pf.seconds());
    for (auto& r : rows) printf("  %.4f MHz  %s  %llu pages  snr %.0f\n", r.absHz / 1e6, r.speed >= 0 ? pagerSpeedName(r.speed) : "none", (unsigned long long)r.pages, r.snrDb);
    CHECK(pf.done(), "the scan did not stop at its limit");
    for (int k = 0; k < 3; k++) {
        const PagerCandidate* c = nullptr;
        for (auto& r : rows) if (std::fabs(r.hz - car[k].hz) < kTol) c = &r;
        CHECK(c != nullptr, "pager %d (%.0f Hz) not found", k, car[k].hz);
        if (!c) continue;
        CHECK(c->speed == car[k].speed, "pager %d: speed %d, expected %d", k, c->speed, car[k].speed);
        CHECK(c->pages >= 3, "pager %d: only %llu pages", k, (unsigned long long)c->pages);
        CHECK(std::fabs(c->absHz - (kCentre + c->hz)) < 1.0, "pager %d: absolute frequency %.0f", k, c->absHz);
    }
    for (auto& r : rows) if (std::fabs(r.hz - tonHz) < kTol) CHECK(r.speed < 0 && r.pages == 0, "the bare carrier was taken for a pager (%s)", pagerSpeedName(r.speed));
    // decoded ones first
    for (size_t i = 1; i < rows.size(); i++) CHECK(!(rows[i].pages > rows[i - 1].pages), "rows not ordered by pages");
    CHECK(pf.configure(kFs, kCentre, 30) && pf.rows().empty() && !pf.done() && pf.seconds() == 0, "configure must start over");
    return testSummary("pager_find");
}
