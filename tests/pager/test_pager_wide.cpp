// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Whole-span pagers: three carriers of different speeds in one 2 Msps capture (POCSAG 512 on the raster, a weak POCSAG 2400 1.2 kHz off it, FLEX 3200/4
// 3 kHz off it, the last one starting late). Every carrier must get its own receiver, every page must arrive with its exact text and address and be
// stamped with the frequency of the carrier that sent it, none may turn up at another carrier; a retune must keep the rows at their absolute
// frequencies; and the single receiver (switch off) is unchanged.
#include "dect2/engine/modes.h"
#include "dect2/pager/pager_gen.h"
#include "dect2/pager/pager_rx.h"
#include "dect2/t2/t2rx.h"
#include "dect2/engine/wide_modes.h"
#include <cmath>
#include <cstdio>
#include <memory>
using namespace dect2;
#include "../common/test_util.h"

static const double kFs = 2.0e6;
static const double kTol = 4000;   // the FLEX 4-level test signal sits about 2.8 kHz off its nominal carrier (its symbol mix is lopsided); the POCSAG ones within 150 Hz

int main() {
    const double off = -pagerTuning().tuneOffsetHz;
    struct Car { int speed; double hz, amp, startSec; };
    const Car car[3] = {{kPocsag512, off - 100000 + 0, 1.0, 0}, {kPocsag2400, off + 50000 - 1200, 0.45, 0}, {kFlex3200_4, off + 175000 + 3000, 0.8, 5.0}};
    std::unique_ptr<ModeSynth> g[3];
    double secs = 0;
    for (int k = 0; k < 3; k++) {
        SynthConfig c; c.snrDb = 30; c.cfoHz = car[k].hz - off; c.modeOpt[2] = car[k].speed + 1; c.modeOpt[3] = 11 + k;
        g[k] = makePagerSynth(c, kFs);
        secs = std::max(secs, pagerCycleSeconds(0, car[k].speed + 1) * 2.3 + car[k].startSec);
    }
    WideModes wm;
    wm.configure(24, kFs, off);
    wm.setCenterHz(466.0e6);
    RxTelemetry t; uint64_t seq = 0;
    const size_t blk = (size_t)(kFs * 0.1);
    std::vector<cf32> sum(blk), tmp(blk);
    size_t peak = 0;
    for (double s = 0; s < secs; s += 0.1) {
        std::fill(sum.begin(), sum.end(), cf32(0, 0));
        for (int k = 0; k < 3; k++) {
            g[k]->generate(tmp.data(), blk);
            if (s < car[k].startSec) continue;
            for (size_t i = 0; i < blk; i++) sum[i] += tmp[i] * (float)car[k].amp;
        }
        wm.feed(24, sum.data(), blk);
        peak = std::max(peak, wm.liveCount(24));
        wm.telemetry(24, t, seq);
    }
    printf("peak %zu decoders, %zu rows, %llu pages\n", peak, t.wide.size(), (unsigned long long)t.pager.messagesTotal);
    for (auto& r : t.wide) printf("  %+9.0f Hz  level %.1f  snr %.1f  state %d  %s\n", r.hz, r.levelDb, r.snrDb, r.state, r.info.c_str());
    CHECK(t.wide.size() == 3, "%zu carriers in the table, expected 3", t.wide.size());
    const std::vector<PagerMessage> none;
    const std::vector<PagerMessage>& got = t.pager.messages ? *t.pager.messages : none;
    for (int k = 0; k < 3; k++) {
        const WideCarrier* row = nullptr; for (auto& r : t.wide) if (std::fabs(r.hz - car[k].hz) < kTol) row = &r;
        CHECK(row != nullptr, "carrier %d (%.0f Hz) not found", k, car[k].hz);
        if (row) CHECK(row->state == 2 && !row->info.empty(), "carrier %d: state %d info '%s'", k, row->state, row->info.c_str());
        const std::vector<PagerTestMessage> want = pagerTestMessages(0, car[k].speed + 1);
        int found = 0;
        for (const PagerTestMessage& w : want) {
            bool ok = false;
            for (const PagerMessage& m : got)
                if (m.carrierId > 0 && std::fabs(m.carrierHz - car[k].hz) < kTol && m.speed == w.speed && m.address == w.address && m.type == w.type && m.text == w.text) { ok = true; break; }
            if (!ok) printf("  carrier %d: missing %s address %u '%s'\n", k, pagerSpeedName(w.speed), w.address, w.text.c_str());
            found += ok;
        }
        printf("  carrier %d (%s, %+.0f Hz): %d of %zu pages\n", k, pagerSpeedName(car[k].speed), car[k].hz, found, want.size());
        CHECK(found == (int)want.size(), "carrier %d: %d of %zu pages", k, found, want.size());
        int wrong = 0;
        for (const PagerMessage& m : got) if (std::fabs(m.carrierHz - car[k].hz) < kTol && m.speed != car[k].speed) wrong++;
        CHECK(wrong == 0, "carrier %d: %d pages of another speed stamped with its frequency", k, wrong);
    }
    for (const PagerMessage& m : got) CHECK(m.carrierId > 0, "page without a carrier stamp");
    // a retune by +500 kHz: the rows keep their absolute frequencies (relative to the new centre they are 500 kHz lower)
    const size_t before = got.size();
    wm.setCenterHz(466.5e6);
    for (int i = 0; i < 5; i++) { std::fill(sum.begin(), sum.end(), cf32(0, 0)); wm.feed(24, sum.data(), blk); }
    wm.telemetry(24, t, seq);
    const std::vector<PagerMessage>& after = t.pager.messages ? *t.pager.messages : none;
    CHECK(after.size() == before && before > 0, "pages after the retune: %zu, before %zu", after.size(), before);
    int moved = 0;
    for (const PagerMessage& m : after) for (int k = 0; k < 3; k++) if (std::fabs(m.carrierHz - (car[k].hz - 500000.0)) < kTol) { moved++; break; }
    CHECK(moved == (int)after.size(), "only %d of %zu pages kept their absolute frequency over the retune", moved, after.size());
    // clear
    wm.clearResults();
    wm.telemetry(24, t, seq);
    CHECK(!t.pager.messages || t.pager.messages->empty(), "clear left pages");

    {   // the single receiver, no whole span: one carrier at the tune offset, all pages, no stamp
        SynthConfig c; c.snrDb = 25; c.modeOpt[2] = kPocsag1200 + 1;
        auto syn = makePagerSynth(c, kFs);
        PagerReceiver rx; rx.configure(kFs); rx.setSignalOffset(off);
        std::vector<cf32> b(65536);
        for (double d = 0; d < pagerCycleSeconds(0, kPocsag1200 + 1) * 2.2 * kFs; d += (double)b.size()) { syn->generate(b.data(), b.size()); rx.feed(b.data(), b.size()); }
        PagerTelemetry pt; rx.telemetry(pt, 0);
        const std::vector<PagerTestMessage> want = pagerTestMessages(0, kPocsag1200 + 1);
        int found = 0;
        for (const PagerTestMessage& w : want) for (const PagerMessage& m : *pt.messages) if (m.address == w.address && m.text == w.text && m.carrierId == 0) { found++; break; }
        CHECK(found == (int)want.size(), "single receiver: %d of %zu pages", found, want.size());
    }
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
