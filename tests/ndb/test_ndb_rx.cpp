// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NDB ident: a carrier 12 kHz below the capture centre (the mode's tune offset), Morse "NDB" at 7 WPM.
// Three paths the beacon band actually uses: 400 Hz AM, 1020 Hz AM, and a keyed carrier.
#include "dect2/ndb/ndb_gen.h"
#include "dect2/ndb/ndb_rx.h"
#include "dect2/ndb/ndb_tel.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;

#include "../common/test_util.h"

static NdbTelemetry run(double toneHz, double secs) {
    const ModeTuning mt = ndbTuning();
    NdbGenConfig c;
    c.rate = mt.sampleRate;
    c.dialOffsetHz = -mt.tuneOffsetHz;
    c.toneHz = toneHz;
    c.ident = "NDB";
    c.wpm = 7;
    c.snrDb = 28;
    c.seed = 3;
    NdbGenerator g(c);
    NdbReceiver r;
    r.setSilent(true);
    r.configure(mt.sampleRate);
    r.setSignalOffset(-mt.tuneOffsetHz);
    std::vector<cf32> x(32768);
    NdbTelemetry last;
    uint64_t seq = 0;
    const double dt = (double)x.size() / mt.sampleRate;
    for (double t = 0; t < secs; t += dt) {
        g.generate(x.data(), x.size());
        r.feed(x.data(), x.size());
        NdbTelemetry tt;
        if (r.telemetry(tt, seq)) { seq = tt.seq; last = std::move(tt); }
    }
    return last;
}

int main() {
    const ModeTuning mt = ndbTuning();
    CHECK(mt.stdMode == 31 && mt.defMhz > 0.19 && mt.defMhz < 0.53, "tuning %d %.3f", mt.stdMode, mt.defMhz);
    CHECK(modeTuningById("ndb") && modeTuningById("ndb")->stdMode == 31, "registry");

    const NdbTelemetry a = run(400, 36);
    CHECK(a.ident == "NDB", "400 Hz ident '%s' text '%s' tone %d wpm %.1f", a.ident.c_str(), a.text.c_str(), a.toneHz, a.wpm);
    CHECK(a.toneHz == 400, "400 Hz path reported %d", a.toneHz);

    const NdbTelemetry b = run(1020, 36);
    CHECK(b.ident == "NDB", "1020 Hz ident '%s' text '%s' tone %d", b.ident.c_str(), b.text.c_str(), b.toneHz);
    CHECK(b.toneHz == 1020, "1020 Hz path reported %d", b.toneHz);

    const NdbTelemetry k = run(0, 36);
    CHECK(k.ident == "NDB", "keyed carrier ident '%s' text '%s' tone %d", k.ident.c_str(), k.text.c_str(), k.toneHz);
    CHECK(k.toneHz < 0, "keyed carrier path reported %d", k.toneHz);

    printf(fails ? "ndb: %d FAILED\n" : "ndb: all passed\n", fails);
    return fails ? 1 : 0;
}
