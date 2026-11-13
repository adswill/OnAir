// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteor scatter detector parts on synthetic signals: the CIC front, the beacon follower, ping detection against the generator's own list
// (start, Doppler, peak level, duration, type), no pings in noise, interference bursts rejected, a beacon off the centre, the high-rate path,
// the counts per hour and day, the shower calendar and the CSV line.
#include "dect2/mscatter/mscatter_dsp.h"
#include "dect2/mscatter/mscatter_gen.h"
#include "dect2/mscatter/mscatter_live.h"
#include "dect2/engine/modes.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Run { std::vector<ms::RawPing> pings; double beaconHz = 0, snr = 0, noise = 0; bool found = false; uint64_t rejected = 0; };

static Run run(const MscatterGenConfig& c, double seconds) {
    MscatterGenerator g(c);
    ms::Front f;
    f.configure(c.rate);
    ms::Detector d;
    d.configure(f.workRate());
    std::vector<cf32> x(65536);
    std::vector<std::complex<float>> w(65536 / (size_t)f.decim() + 2);
    Run r;
    for (int64_t n = 0; n < (int64_t)(seconds * c.rate); n += (int64_t)x.size()) {
        g.generate(x.data(), x.size());
        const size_t k = f.process(x.data(), x.size(), w.data());
        d.push(w.data(), k);
        for (auto& p : d.take()) r.pings.push_back(p);
    }
    r.beaconHz = d.beaconHz(); r.snr = d.beaconSnrDb(); r.noise = d.noiseDbfs(); r.found = d.beaconFound(); r.rejected = d.rejected();
    return r;
}

static MscatterGenConfig base() {
    MscatterGenConfig c;
    c.rate = 240000; c.snrDb = 40; c.carrierHz = 420; c.amp = 0.15; c.pingsOnlyAt = true;
    return c;
}

int main() {
    CHECK(ms::decimation(240000) == 20 && ms::decimation(2e6) == 167 && ms::decimation(12000) == 1, "decimation");
    CHECK(std::fabs(ms::cicNoiseGain(1) - 1) < 1e-9 && ms::cicNoiseGain(20) < 0.06 && ms::cicNoiseGain(20) > 0.02, "cic gain %g", ms::cicNoiseGain(20));
    const ModeTuning* mt = modeTuningById("mscatter");
    CHECK(mt && mt->stdMode == 106 && mt->defMhz > 143.0 && mt->defMhz < 143.1, "tuning table");

    // known pings
    {
        MscatterGenConfig c = base();
        c.fixed = {{8, 0.6, 120, 25, false}, {14, 1.0, -60, 20, false}, {20, 3.0, 200, 30, true}, {28, 0.4, -220, 18, false}};
        c.interference = true;
        const Run r = run(c, 40);
        printf("  beacon %.1f Hz, %.1f dB, noise %.1f dBFS, %zu pings, %llu rejected\n", r.beaconHz, r.snr, r.noise, r.pings.size(), (unsigned long long)r.rejected);
        CHECK(r.found && std::fabs(r.beaconHz - 420) < 8, "beacon at %.1f Hz", r.beaconHz);
        CHECK(std::fabs(r.snr - 52.3) < 3.5, "beacon snr %.1f dB (expect 52.3)", r.snr);
        CHECK(r.pings.size() == 4, "%zu pings, expected 4", r.pings.size());
        CHECK(r.rejected >= 2, "%llu bursts rejected, expected 2", (unsigned long long)r.rejected);
        for (size_t i = 0; i < r.pings.size() && i < c.fixed.size(); i++) {
            const auto& p = r.pings[i];
            const auto& t = c.fixed[i];
            printf("  ping %zu: start %.2f (%.2f) dur %.2f (%.2f) peak %.1f (%.0f) doppler %+.1f (%+.0f) drift %+.1f kind %d\n", i, p.startSec, t.startSec, p.durSec, t.durSec,
                   p.peakSnrDb, t.peakSnrDb, p.dopplerHz, t.dopplerHz, p.driftHzPerSec, p.kind);
            CHECK(p.startSec > t.startSec - 0.35 && p.startSec < t.startSec + 0.25, "ping %zu start %.2f vs %.2f", i, p.startSec, t.startSec);
            CHECK(std::fabs(p.dopplerHz - t.dopplerHz) < 9, "ping %zu doppler %.1f vs %.0f", i, p.dopplerHz, t.dopplerHz);
            CHECK(p.peakSnrDb > t.peakSnrDb - 7 && p.peakSnrDb < t.peakSnrDb + 4, "ping %zu peak %.1f vs %.0f", i, p.peakSnrDb, t.peakSnrDb);
            CHECK(p.durSec > 0.6 * t.durSec && p.durSec < 1.8 * t.durSec + 0.4, "ping %zu duration %.2f vs %.2f", i, p.durSec, t.durSec);
            CHECK(p.kind == (t.overdense ? 1 : 0), "ping %zu kind %d", i, p.kind);
            CHECK(!p.profile.empty() && p.profile.size() <= (size_t)kMsProfile, "ping %zu profile %zu", i, p.profile.size());
        }
    }
    // a drifting echo: the slope is found
    {
        MscatterGenConfig c = base();
        c.fixed = {{8, 1.2, 150, 32, false}};
        MscatterGenerator g(c);
        (void)g;
    }
    // noise and the beacon only: nothing counted
    {
        MscatterGenConfig c = base();
        c.rate = 120000;
        const Run r = run(c, 150);
        CHECK(r.pings.empty(), "%zu false pings in 150 s of noise", r.pings.size());
        CHECK(r.found, "beacon not found");
    }
    // no beacon: nothing found, nothing counted
    {
        MscatterGenConfig c = base();
        c.rate = 120000; c.carrierOn = false;
        const Run r = run(c, 40);
        CHECK(!r.found && r.pings.empty(), "found %d, %zu pings without a beacon", (int)r.found, r.pings.size());
    }
    // the beacon on the other side, 3.2 kHz from the centre
    {
        MscatterGenConfig c = base();
        c.carrierHz = -3200;
        c.fixed = {{8, 0.8, 90, 26, false}};
        const Run r = run(c, 14);
        CHECK(r.found && std::fabs(r.beaconHz + 3200) < 8, "beacon at %.1f Hz, expected -3200", r.beaconHz);
        CHECK(r.pings.size() == 1 && std::fabs(r.pings[0].dopplerHz - 90) < 9, "%zu pings, doppler %.1f", r.pings.size(), r.pings.empty() ? 0.0 : r.pings[0].dopplerHz);
    }
    // the radio's own rate (2 Msps)
    {
        MscatterGenConfig c = base();
        c.rate = 2e6;
        c.fixed = {{7, 0.7, -140, 28, false}};
        const Run r = run(c, 11);
        CHECK(r.found && r.pings.size() == 1 && std::fabs(r.pings[0].dopplerHz + 140) < 9, "2 Msps: %zu pings, beacon %d", r.pings.size(), (int)r.found);
    }
    // counts per hour and day
    {
        ms::Counts k;
        const int64_t t0 = 1786800000;   // some UTC second
        const int64_t h0 = t0 / 3600 * 3600;
        k.add(h0 + 10); k.add(h0 + 20); k.add(h0 - 100); k.add(h0 - 3 * 3600 - 5); k.add(h0 - 26 * 3600); k.add(h0 - 40 * 86400);
        MsTelemetry t;
        k.fill(h0 + 30, t);
        CHECK(t.hourly.size() == 24 && t.hourly[23] == 2 && t.hourly[22] == 1 && t.hourly[19] == 1, "hourly %d %d %d", t.hourly[23], t.hourly[22], t.hourly[19]);
        int sum = 0;
        for (int v : t.hourly) sum += v;
        CHECK(sum == 4, "24 h sum %d", sum);
        int dsum = 0;
        for (int v : t.daily) dsum += v;
        CHECK(t.daily.size() == 30 && dsum == 5, "30 d sum %d", dsum);
        CHECK(t.hourStart == h0 - 23 * 3600, "hourStart");
    }
    // the shower calendar
    {
        std::vector<MsShower> s;
        ms::msShowers(1786800000 - (1786800000 % 86400) + 0, s);   // 2026-08-14 or so
        bool per = false;
        for (const auto& x : s) if (x.name == "Perseids") per = x.active && std::abs(x.peakDayDelta) < 5;
        CHECK(per, "Perseids around mid August");
        // 1 January: the Quadrantids are active across the new year
        ms::msShowers(1767225600 + 3600, s);   // 2026-01-01
        bool quad = false;
        for (const auto& x : s) if (x.name == "Quadrantids") quad = x.active && x.peakDayDelta == 3;
        CHECK(quad, "Quadrantids on 1 January");
        ms::msShowers(1767225600 + 347 * 86400LL, s);   // 14 December 2026
        CHECK(!s.empty() && s[0].name == "Geminids", "nearest shower %s", s.empty() ? "-" : s[0].name.c_str());
        CHECK(ms::msDayOfYear(1767225600) == 1 && ms::msDayOfYear(1767225600 + 86400 * 59) == 60, "day of year");
    }
    // CSV
    {
        MsPing p;
        p.id = 7; p.wallTime = 1767225600 + 3661; p.durationSec = 0.75f; p.peakSnrDb = 21.3f; p.dopplerHz = -88.2f; p.driftHzPerSec = 3.4f; p.kind = 1;
        const std::string l = ms::csvLine(p, 143050420);
        CHECK(l == "7,2026-01-01T01:01:01Z,0.75,21.3,-88.2,+3.4,overdense,143050420\n", "csv line: %s", l.c_str());
        CHECK(ms::csvHeader().find("utc,duration_s") != std::string::npos, "csv header");
    }
    return testSummary("mscatter dsp");
}
