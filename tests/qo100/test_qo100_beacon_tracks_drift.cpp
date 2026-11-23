// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The beacon correction of the QO-100 tool on a hard LNB (the 4.5 report: "the frequency keeps jumping around"): the narrowband
// transponder at 2 Msps through an LNB that drifts 175 Hz/min with jitter, with a strong carrier sweeping through the lower beacon, and
// through an LNB whose oscillator steps by 250 Hz. The smooth correction must stay within 5 Hz of the LNB's real error once settled and
// never move by more than the slew limit between two reports. Also the tracker alone: outliers are gated, a step needs agreement.
#include "dect2/qo100/qo100_dsp.h"
#include "dect2/qo100/qo100_gen.h"
#include "dect2/qo100/qo100_rx.h"
#include "dect2/qo100/qo100_tel.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Out { double lockAt = -1, worst = 0, maxJump = 0; int samples = 0; bool lostLock = false; };

static Out run(const SynthConfig& sc, const Qo100SynthOpts& o, double secs, double settleAfter, double settleFrom = 0, double settleTo = 0) {
    Out r;
    const double fs = 2e6, lo = 9750e6;
    Qo100Receiver rx;
    rx.configure(fs);
    rx.setLo(lo);
    rx.setCenterHz(qo100::ifOf(qo100::kNarrowCentreHz, lo));
    auto syn = makeQo100SynthEx(sc, o, fs);
    std::vector<cf32> buf(20000);
    uint64_t seq = 0, done = 0;
    double prev = 0; bool havePrev = false;
    while (done < (uint64_t)(secs * fs)) {
        syn->generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        done += buf.size();
        Qo100Telemetry t;
        if (!rx.telemetry(t, seq)) continue;
        seq = t.seq;
        const double now = (double)done / fs;
        if (t.state == 2 && r.lockAt < 0) r.lockAt = now;
        if (r.lockAt >= 0 && !t.corrValid) r.lostLock = true;
        if (!t.corrValid) continue;
        if (havePrev) r.maxJump = std::max(r.maxJump, std::fabs(t.lnbErrHz - prev));
        prev = t.lnbErrHz; havePrev = true;
        const bool settled = r.lockAt >= 0 && now > r.lockAt + settleAfter && !(now >= settleFrom && now < settleTo);
        if (settled) { r.worst = std::max(r.worst, std::fabs(t.lnbErrHz - qo100SynthErrHz(sc, o, now))); r.samples++; }
    }
    return r;
}

int main() {
    // the tracker alone
    {
        qo100::BeaconTracker tr;
        double t = 0, truth = 1000;
        bool locked = false;
        for (int i = 0; i < 60; i++) {
            t += 0.6; truth += 1.5 * 0.6;
            qo100::BeaconMeas m[2];
            m[0].ok = m[1].ok = true; m[0].errHz = truth + 0.4; m[1].errHz = truth - 0.4;
            if (i == 30) m[0].errHz += 300;                  // one beacon has a carrier on it
            if (i >= 40 && i < 43) m[0].errHz += 200, m[1].errHz += 200;   // two bad epochs agreeing: too few to take as a step
            tr.update(t, m);
            if (tr.locked()) locked = true;
            const double v = tr.value(t);
            if (locked && i > 6) CHECK(std::fabs(v - truth) < 3.0, "tracker epoch %d: %.2f Hz off", i, v - truth);
        }
        CHECK(locked, "the tracker never locked");
        CHECK(std::fabs(tr.rateHzPerSec() - 1.5) < 0.3, "drift rate %.2f Hz/s", tr.rateHzPerSec());
    }
    const double kDrift = 175;
    // 1: drift + jitter + a strong carrier sweeping through the lower beacon
    {
        SynthConfig sc; sc.modeVal[1] = kDrift;
        Qo100SynthOpts o; o.jitterHz = 1.0; o.sweepHzPerSec = 60; o.sweepStartHz = -1500;
        const Out r = run(sc, o, 45, 3);
        printf("  drift: locked at %.1f s, worst error %.2f Hz over %d reports, largest move %.2f Hz\n", r.lockAt, r.worst, r.samples, r.maxJump);
        CHECK(r.lockAt > 0 && r.lockAt < 6, "locked at %.1f s", r.lockAt);
        CHECK(r.samples > 200 && r.worst < 5.0, "the corrected beacon is %.2f Hz off the real one", r.worst);
        CHECK(r.maxJump < 3.0, "the correction jumped by %.2f Hz between two reports", r.maxJump);
        CHECK(!r.lostLock, "the lock was dropped");
    }
    // 2: the oscillator steps by 250 Hz at 20 s: taken after a few agreeing epochs, then slewed, not jumped; settled again within 8 s
    {
        SynthConfig sc; sc.modeVal[1] = kDrift;
        Qo100SynthOpts o; o.jitterHz = 1.0; o.stepHz = 250; o.stepAtSec = 20;
        const Out r = run(sc, o, 45, 3, 20, 28);
        printf("  step: worst error %.2f Hz outside the step window, largest move %.2f Hz\n", r.worst, r.maxJump);
        CHECK(r.samples > 150 && r.worst < 5.0, "the corrected beacon is %.2f Hz off the real one", r.worst);
        CHECK(r.maxJump < 12.0, "the correction jumped by %.2f Hz between two reports (slew limit 10 Hz per report)", r.maxJump);
    }
    return testSummary("qo100 beacon tracks drift");
}
