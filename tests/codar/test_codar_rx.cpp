// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CODAR / HF ocean radar monitor: the test signal through the receiver (codar_gen.h, codar_rx.h): three radars with their width, rate,
// direction and repetition, the call sign, noise alone, a carrier and a wide ionosonde chirp (nothing to count), other sample rates, a radio
// on a band (band and absolute frequency), the scan plan, and the results file.
#include "dect2/codar/codar_gen.h"
#include "dect2/codar/codar_rx.h"
#include "dect2/codar/codar_tel.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static CodarTelemetry run(CodarGenConfig c, double secs, double centerHz = 0, CodarReceiver* keep = nullptr) {
    CodarGenerator g(c);
    CodarReceiver local;
    CodarReceiver& r = keep ? *keep : local;
    r.configure(c.rate);
    r.setCenterHz(centerHz);
    std::vector<cf32> buf(16384);
    const size_t total = (size_t)((secs + 0.3) * c.rate);
    for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    CodarTelemetry t;
    r.telemetry(t, 0);
    return t;
}

static const CdStation* nearBy(const CodarTelemetry& t, double offKhz, double bwKhz) {
    for (const auto& s : t.stations) if (std::fabs(s.offsetKhz - offKhz) < 5 && std::fabs(s.bwKhz - bwKhz) < 0.15 * bwKhz + 2) return &s;
    return nullptr;
}

int main() {
    CodarGenConfig c;
    c.rate = 2e6;
    c.snrDb = 22;
    c.radars = codarTestRadars(false);
    {   // the schedule, radio on the 12 - 14 MHz band
        const CodarTelemetry t = run(c, 12, 13.0e6);
        printf("  schedule: %zu radars, %llu sweeps, %llu rejected: %s\n", t.stations.size(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, codarSummary(t).c_str());
        for (const auto& s : t.stations) printf("    #%d off %.1f kHz bw %.1f rate %.1f dir %d period %.3f sweeps %llu snr %.1f\n", s.id, s.offsetKhz, s.bwKhz, s.rateKhzS, s.dir, s.periodSec, (unsigned long long)s.sweeps, s.snrDb);
        CHECK(t.stations.size() == 3, "%zu radars", t.stations.size());
        const CdStation* a = nearBy(t, -300, 50);
        const CdStation* b = nearBy(t, 250, 100);
        const CdStation* d = nearBy(t, 550, 25);
        CHECK(a && b && d, "radars found %d %d %d", a != nullptr, b != nullptr, d != nullptr);
        if (a) {
            CHECK(a->dir == 1 && std::fabs(a->periodSec - 0.5) < 0.02, "radar A dir %d period %.3f", a->dir, a->periodSec);
            CHECK(std::fabs(a->rateKhzS - 50.0 / 0.45) < 12, "radar A rate %.1f", a->rateKhzS);
            CHECK(std::fabs(a->centreMhz - 12.7) < 0.005 && a->band == "12 - 14 MHz", "radar A %.4f MHz, %s", a->centreMhz, a->band.c_str());
            CHECK(a->sweeps >= 15 && a->snrDb > 8, "radar A %llu sweeps, %.1f dB", (unsigned long long)a->sweeps, a->snrDb);
            CHECK(a->hint.find("possible") == 0 && a->callsign.empty(), "hint '%s' id '%s'", a->hint.c_str(), a->callsign.c_str());
            CHECK(!a->snrHist.empty() && a->firstWall > 0 && a->lastWall >= a->firstWall, "history");
        }
        if (b) CHECK(b->dir == -1 && std::fabs(b->periodSec - 1.0) < 0.03, "radar B dir %d period %.3f", b->dir, b->periodSec);
        if (d) CHECK(d->dir == 1 && std::fabs(d->periodSec - 0.5) < 0.02 && std::fabs(d->bwKhz - 25) < 5, "radar C dir %d period %.3f bw %.1f", d->dir, d->periodSec, d->bwKhz);
        CHECK(t.state == 1 && t.bandNow == "12 - 14 MHz", "state %d band '%s'", t.state, t.bandNow.c_str());
        CHECK(!t.events.empty() && t.events[0].band == "12 - 14 MHz", "events %zu", t.events.size());
        CHECK(t.wf.size() == (size_t)kCdWfRows * kCdWfCols && !t.sweepMarks.empty(), "waterfall %zu, marks %zu", t.wf.size(), t.sweepMarks.size());
        size_t lit = 0;
        for (uint8_t v : t.wf) if (v > 120) lit++;
        CHECK(lit > 200, "waterfall shows the sweeps: %zu bright cells", lit);
    }
    {   // the call sign: a 12 s cycle (6 s of sweeps, then the ID)
        CodarGenConfig ci = c;
        ci.radars = codarTestRadars(true);
        const CodarTelemetry t = run(ci, 30, 13.0e6);
        const CdStation* a = nearBy(t, -300, 50);
        CHECK(a != nullptr, "radar A");
        if (a) {
            printf("  ID: '%s' seen %d times at %.1f wpm, unverified %d\n", a->callsign.c_str(), a->idSeen, a->idWpm, a->idUnverified);
            CHECK(a->callsign == "AB1CD" && a->idUnverified && a->idSeen >= 2, "call sign '%s'", a->callsign.c_str());
            CHECK(std::fabs(a->idWpm - 15) < 3, "wpm %.1f", a->idWpm);
        }
        const CdStation* b = nearBy(t, 250, 100);
        CHECK(b && b->callsign.empty(), "no ID on radar B");
        CHECK(t.stations.size() == 3, "%zu radars with the ID in the signal", t.stations.size());
    }
    {   // noise alone, and a carrier with a wide chirp: nothing to report
        CodarGenConfig cn = c; cn.noiseOnly = true;
        CodarTelemetry t = run(cn, 8);
        CHECK(t.stations.empty() && t.blocksOk == 0, "noise: %zu radars %llu sweeps", t.stations.size(), (unsigned long long)t.blocksOk);
        CodarGenConfig cj = c; cj.radars.clear(); cj.junk = true;
        t = run(cj, 14);
        CHECK(t.stations.empty(), "carrier and wide chirp: %zu radars", t.stations.size());
    }
    {   // other sample rates, the radar off the centre
        for (double fs : {250e3, 1e6, 4e6}) {
            CodarGenConfig cr = c;
            cr.rate = fs;
            cr.radars.assign(1, codarTestRadars(false)[0]);
            cr.radars[0].offsetHz = fs > 500e3 ? 100e3 : 20e3;
            const CodarTelemetry t = run(cr, 8, 4.5e6);
            const CdStation* a = nearBy(t, cr.radars[0].offsetHz / 1e3, 50);
            CHECK(t.stations.size() == 1 && a, "%.0f ksps: %zu radars", fs / 1e3, t.stations.size());
            if (a) CHECK(a->dir == 1 && std::fabs(a->periodSec - 0.5) < 0.03 && a->band == "4.4 - 4.65 MHz", "%.0f ksps: dir %d period %.3f band %s", fs / 1e3, a->dir, a->periodSec, a->band.c_str());
        }
    }
    {   // the results file: stations stay through clear -> load
        CodarReceiver r;
        const CodarTelemetry t = run(c, 6, 13.0e6, &r);
        const std::string blob = r.saveResults();
        CodarReceiver r2;
        r2.configure(2e6);
        CHECK(r2.loadResults(blob) && !r2.loadResults("garbage"), "load");
        std::vector<cf32> z(65536);
        r2.feed(z.data(), z.size());
        CodarTelemetry l;
        r2.telemetry(l, 0);
        CHECK(l.stations.size() == t.stations.size() && !l.events.empty(), "loaded %zu of %zu radars", l.stations.size(), t.stations.size());
        r.clearResults();
        CodarTelemetry e;
        r.telemetry(e, 0);
        CHECK(e.stations.empty() && e.events.empty(), "cleared");
    }
    {   // the tuning entry
        const ModeTuning mt = codarTuning();
        CHECK(mt.stdMode == 74 && modeTuningById("codar") && modeTuningById("codar")->stdMode == 74, "tuning table");
        CHECK(minSampleRateFor(74, 4) == 240000, "minimum rate");
    }
    printf(fails ? "codar rx: %d FAILED\n" : "codar rx: all passed\n", fails);
    return fails ? 1 : 0;
}
