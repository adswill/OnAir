// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The station fixed on the Time signals receiver (TimeReceiver::setStation, the Station choice of the Time signals screen). The test signal of each station (time_gen.h, a known time, started part way into a minute)
// goes into a receiver fixed to that station, with a carrier error and a sample clock error: the station is the one of the entry, the decoded time is right (DCF77, MSF, WWVB, JJY 40 / 60, WWV, BPM), the first time comes after the first whole minute;
// a signal of another station gives no time in a fixed receiver (DCF77 in the MSF entry, WWVB in the DCF77 entry, JJY 60 in the WWVB entry), and noise alone gives none in any entry. BPC, HLA, BPM coded and RWM are run too
// and checked for the station only (their decoding is covered by test_time_burst / test_time_rwm). Simulated signals only.
#include "dect2/dsp/gen_util.h"
#include "dect2/time/time_gen.h"
#include "dect2/time/time_proto.h"
#include "dect2/time/time_rx.h"
#include "dect2/time/time_tel.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::timesig;
#include "../common/test_util.h"

static const double kLat = 54.687, kLon = 25.280;

static TimeTelemetry run(TimeGenConfig c, int fixedStation, double secs, bool noiseOnly = false) {
    c.dialOffsetHz = -timeTuning().tuneOffsetHz;
    double la, lo; stationSite(c.station, la, lo, c.wwvh);
    c.delayMs = propagationMs(c.station, distanceKm(kLat, kLon, la, lo));
    TimeGenerator g(c);
    TimeReceiver r;
    r.configure(c.rate);
    r.setSignalOffset(-timeTuning().tuneOffsetHz);
    r.setStation(fixedStation);
    r.setClockMode(2, c.startUtc);
    r.setReceiverLocation(kLat, kLon, true);
    genutil::NoiseSource ns(7);
    std::vector<cf32> buf(8192);
    TimeTelemetry last; uint64_t seq = 0;
    for (size_t done = 0; done < (size_t)(secs * c.rate); done += buf.size()) {
        if (noiseOnly) { for (auto& v : buf) v = 0; ns.add(buf.data(), buf.size(), 0.05f); } else g.generate(buf.data(), buf.size());
        r.feed(buf.data(), buf.size());
        TimeTelemetry t; if (r.telemetry(t, seq)) { seq = t.seq; last = std::move(t); }
    }
    return last;
}

int main() {
    struct V { const char* id; const char* name; int station; };   // the filters the Time signals screen offers
    static const V views[] = {
        {"dcf77", "DCF77 (Germany, 77.5 kHz)", 0},
        {"msf", "MSF (United Kingdom, 60 kHz)", 1},
        {"wwvb", "WWVB (United States, 60 kHz)", 2},
        {"jjy40", "JJY 40 kHz (Japan)", 3},
        {"jjy60", "JJY 60 kHz (Japan)", 4},
        {"wwv", "WWV / WWVH (United States, HF)", 5},
        {"bpm", "BPM (China, HF)", 7},
        {"bpc", "BPC (China, 68.5 kHz)", 8},
        {"hla", "HLA (Korea, 5 MHz)", 9},
        {"bpmcoded", "BPM coded (China, HF)", 10},
        {"rwm", "RWM (Russia, 9996 kHz)", 11},
    };
    const int nv = (int)(sizeof views / sizeof *views);

    const double start = (double)(daysFromCivil(2026, 10, 12) * 86400 + 14 * 3600 + 2 * 60 + 37);   // 14:02:37 UTC
    for (int i = 0; i < nv; i++) {
        const int st = views[i].station;
        TimeGenConfig c; c.station = st; c.rate = 64000; c.snrDb = 30; c.cfoHz = (i % 2) ? 6.0 : -4.0; c.clockPpm = (i % 3) ? 8.0 : -12.0; c.startUtc = start; c.opts.dut1 = -2;
        const double secs = 175;
        const TimeTelemetry t = run(c, st, secs);
        printf("  %-14s %s, ok %llu bad %llu, station %s\n", views[i].id, timeSummary(t).c_str(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, stationName(t.station));
        CHECK(t.station == st, "%s: station %d", views[i].id, t.station);
        const bool decoded = st == Dcf77 || st == Msf || st == Wwvb || st == Jjy40 || st == Jjy60 || st == Wwv || st == Bpm;
        if (decoded) {
            CHECK(t.timeValid, "%s: no time", views[i].id);
            if (t.timeValid) { const double expect = start + secs - c.delayMs * 1e-3; CHECK(std::fabs(t.utcNow - expect) < 1.5 + 0.0, "%s: time %.1f s off", views[i].id, t.utcNow - expect); CHECK(t.blocksBad == 0, "%s: %llu minutes rejected", views[i].id, (unsigned long long)t.blocksBad); }
        }
    }
    // another station's signal in a fixed receiver: no time
    struct X { int signal, entry; };
    for (const X& x : {X{Dcf77, Msf}, X{Wwvb, Dcf77}, X{Jjy60, Wwvb}}) {
        TimeGenConfig c; c.station = x.signal; c.rate = 64000; c.snrDb = 30; c.startUtc = start;
        const TimeTelemetry t = run(c, x.entry, 175);
        printf("  %s signal in the %s entry: %s\n", stationName(x.signal), stationName(x.entry), timeSummary(t).c_str());
        CHECK(!t.timeValid && t.blocksOk == 0, "%s signal in the %s entry gave a time", stationName(x.signal), stationName(x.entry));
    }
    // noise alone in every entry
    for (int i = 0; i < nv; i++) {
        if (views[i].station == Jjy40) continue;
        TimeGenConfig c; c.station = views[i].station; c.rate = 64000; c.startUtc = 1.7e9;
        const TimeTelemetry t = run(c, views[i].station, 150, true);
        CHECK(t.blocksOk == 0 && !t.timeValid, "%s: a time from noise", views[i].id);
    }
    return testSummary("time views");
}
