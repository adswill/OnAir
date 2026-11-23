// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The beacon correction when the beacons fade: all three beacons vanish for 5 s while the LNB keeps drifting (175 Hz/min, jitter). The
// correction must carry on with the drift (within 8 Hz of the real error all through the gap), never jump, show "held" and not lose
// the lock, and be back on the beacons within 5 Hz after they return, with no jump when they do.
#include "dect2/qo100/qo100_dsp.h"
#include "dect2/qo100/qo100_gen.h"
#include "dect2/qo100/qo100_rx.h"
#include "dect2/qo100/qo100_tel.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const double fs = 2e6, lo = 9750e6, secs = 45, dropAt = 20, dropLen = 5;
    SynthConfig sc; sc.modeVal[1] = 175;
    Qo100SynthOpts o; o.jitterHz = 1.0; o.dropAtSec = dropAt; o.dropSec = dropLen;
    Qo100Receiver rx;
    rx.configure(fs);
    rx.setLo(lo);
    rx.setCenterHz(qo100::ifOf(qo100::kNarrowCentreHz, lo));
    auto syn = makeQo100SynthEx(sc, o, fs);
    std::vector<cf32> buf(20000);
    uint64_t seq = 0, done = 0;
    double lockAt = -1, prev = 0, maxJump = 0, worstGap = 0, worstAfter = 0;
    bool havePrev = false, held = false, lostCorr = false, backLocked = false;
    int nGap = 0, nAfter = 0;
    while (done < (uint64_t)(secs * fs)) {
        syn->generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        done += buf.size();
        Qo100Telemetry t;
        if (!rx.telemetry(t, seq)) continue;
        seq = t.seq;
        const double now = (double)done / fs;
        if (t.state == 2 && lockAt < 0) lockAt = now;
        if (lockAt < 0) continue;
        if (!t.corrValid) lostCorr = true;
        if (now > dropAt && now < dropAt + dropLen && t.state == 3) held = true;
        if (now > dropAt + dropLen + 8 && t.state == 2) backLocked = true;
        if (havePrev) maxJump = std::max(maxJump, std::fabs(t.lnbErrHz - prev));
        prev = t.lnbErrHz; havePrev = true;
        const double e = std::fabs(t.lnbErrHz - qo100SynthErrHz(sc, o, now));
        if (now > dropAt + 0.5 && now < dropAt + dropLen + 1) { worstGap = std::max(worstGap, e); nGap++; }
        else if (now > dropAt + dropLen + 4) { worstAfter = std::max(worstAfter, e); nAfter++; }
    }
    printf("  dropout: locked at %.1f s, error in the gap %.2f Hz, after %.2f Hz, largest move %.2f Hz, held %d\n", lockAt, worstGap, worstAfter, maxJump, held);
    CHECK(lockAt > 0 && lockAt < 6, "locked at %.1f s", lockAt);
    CHECK(nGap > 30 && worstGap < 8.0, "in the gap the correction was %.2f Hz off the real error", worstGap);
    CHECK(nAfter > 100 && worstAfter < 5.0, "after the beacons came back it was %.2f Hz off", worstAfter);
    CHECK(maxJump < 3.0, "the correction jumped by %.2f Hz between two reports", maxJump);
    CHECK(held, "the status never showed the correction held while the beacons were out");
    CHECK(!lostCorr, "the correction was dropped");
    CHECK(backLocked, "not locked again after the beacons came back");
    return testSummary("qo100 beacon no jumps on dropout");
}
