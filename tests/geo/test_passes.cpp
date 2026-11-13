// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Satellite pass planner core: TLE parsing, SGP4 against the Vallado et al. (2006) verification vectors, GMST, pass search
// (an observer put under the satellite must see it culminate overhead at that moment) and Doppler sign and size at 137 MHz.
#include "dect2/geo/passes.h"
#include <cmath>
#include <cstdio>
#include <string>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// put the right checksum in column 69
static std::string fix(std::string l) {
    l.resize(68, ' ');
    int s = 0;
    for (char c : l) { if (c >= '0' && c <= '9') s += c - '0'; else if (c == '-') s++; }
    return l + char('0' + s % 10);
}

static double dist(const double a[3], const double b[3]) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }

int main() {
    // ---- SGP4 verification (tcppver.out of Vallado's SGP4 distribution)
    {
        Tle t;
        const bool ok = parseTle("1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753",
                                 "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667", t, "VANGUARD 1");
        CHECK(ok, "00005 parses");
        CHECK(t.norad == 5 && std::fabs(t.ecc - 0.1859667) < 1e-9 && std::fabs(t.bstar - 0.28098e-4) < 1e-12, "fields %d %g %g", t.norad, t.ecc, t.bstar);
        Sgp4 s;
        CHECK(s.init(t), "00005 init");
        double r[3], v[3];
        s.propagate(0, r, v);
        const double r0[3] = {7022.46529266, -1400.08296755, 0.03995155}, v0[3] = {1.893841015, 6.405893759, 4.534807250};
        CHECK(dist(r, r0) < 0.01 && dist(v, v0) < 1e-5, "00005 t=0 r %.6f %.6f %.6f v %.9f %.9f %.9f", r[0], r[1], r[2], v[0], v[1], v[2]);
        s.propagate(360, r, v);
        const double r1[3] = {-7154.03120202, -3783.17682504, -3536.19412294}, v1[3] = {4.741887409, -4.151817765, -2.093935425};
        CHECK(dist(r, r1) < 0.01 && dist(v, v1) < 1e-5, "00005 t=360 r %.6f %.6f %.6f v %.9f %.9f %.9f", r[0], r[1], r[2], v[0], v[1], v[2]);
    }
    {
        Tle t;
        CHECK(parseTle("1 06251U 62025E   06176.82412014  .00008885  00000-0  12808-3 0  3985",
                       "2 06251  58.0579  54.0425 0030035 139.1568 221.1854 15.56387291  6774", t), "06251 parses");
        Sgp4 s;
        CHECK(s.init(t), "06251 init");
        double r[3], v[3];
        s.propagate(0, r, v);
        const double r0[3] = {3988.31022699, 5498.96657235, 0.90055879}, v0[3] = {-3.290032738, 2.357652820, 6.496623475};
        CHECK(dist(r, r0) < 0.01 && dist(v, v0) < 1e-5, "06251 t=0 r %.6f %.6f %.6f v %.9f %.9f %.9f", r[0], r[1], r[2], v[0], v[1], v[2]);
    }
    // ---- parsing: names, CR LF, a bad checksum, a deep-space orbit
    {
        const std::string l1 = fix("1 33591U 09005A   24275.50000000  .00000100  00000-0  70000-4 0  999"), l2 = fix("2 33591  99.1000 300.0000 0013000 200.0000 160.0000 14.12500000800000");
        std::string bad = l2; bad[68] = char('0' + (bad[68] - '0' + 1) % 10);
        const std::vector<Tle> v = parseTleText("NOAA 19\r\n" + l1 + "\r\n" + l2 + "\r\n\r\n" + l1 + "\n" + bad + "\n" + l1 + "\n" + l2 + "\n");
        CHECK(v.size() == 2 && v[0].name == "NOAA 19" && v[1].name == "33591", "parsed %zu", v.size());
        Tle geo;
        CHECK(parseTle(fix("1 40000U 14001A   24275.50000000  .00000000  00000-0  00000-0 0  999"), fix("2 40000   0.0500 100.0000 0002000 200.0000 160.0000  1.00270000 10000"), geo), "GEO parses");
        Sgp4 s;
        CHECK(!s.init(geo), "deep space is refused");
    }
    // ---- time
    CHECK(std::fabs(unixToJd(0) - 2440587.5) < 1e-9, "unix epoch");
    CHECK(std::fabs(gmst(2451545.0) * 180 / M_PI - 280.46061837) < 1e-6, "GMST J2000 %.8f", gmst(2451545.0) * 180 / M_PI);
    // ---- a pass: an observer put under the satellite at time T sees it culminate near the zenith at T
    {
        const std::string l1 = fix("1 33591U 09005A   24275.50000000  .00000100  00000-0  70000-4 0  999"), l2 = fix("2 33591  99.1000 300.0000 0013000 200.0000 160.0000 14.12500000800000");
        Tle t;
        CHECK(parseTle(l1, l2, t, "NOAA 19"), "NOAA parses");
        Sgp4 s;
        CHECK(s.init(t), "NOAA init");
        const double T = jdToUnix(t.epochJd) + 0.3 * 86400 + 1234.5;
        Observer o;
        double alt = 0;
        CHECK(subPoint(s, T, o.latDeg, o.lonDeg, alt) && alt > 800 && alt < 900, "sub point alt %.1f", alt);
        const std::vector<Pass> ps = findPasses(s, o, T - 3600, T + 3600);
        const Pass* p = nullptr;
        for (const auto& q : ps) if (q.aos <= T && q.los >= T) p = &q;
        CHECK(p != nullptr, "pass at T found (%zu passes)", ps.size());
        if (p) {
            CHECK(std::fabs(p->tca - T) < 30, "TCA %.1f s from T", p->tca - T);
            CHECK(p->maxElDeg > 85, "max el %.2f", p->maxElDeg);
            const double dur = p->los - p->aos;
            CHECK(dur > 13 * 60 && dur < 17 * 60, "overhead pass lasts %.0f s", dur);
            // AOS against a plain one-second scan of the elevation
            double scanAos = 0;
            for (double x = T; x > T - 1800; x -= 1) { Look l; lookAt(s, o, x, l); if (l.elDeg < 0) { scanAos = x + 1; break; } }
            CHECK(std::fabs(scanAos - p->aos) < 30 && std::fabs(scanAos - p->aos) < 2, "AOS %.1f vs scan %.1f", p->aos, scanAos);
            CHECK(std::fabs(std::fmod(p->losAzDeg - p->aosAzDeg + 360, 360) - 180) < 20, "rises and sets on opposite sides: %.1f %.1f", p->aosAzDeg, p->losAzDeg);
            // Doppler at 137.1 MHz: higher while it comes, lower while it goes, about 3 kHz at the horizon, none overhead
            Look la, lt, ll;
            lookAt(s, o, p->aos + 5, la); lookAt(s, o, p->tca, lt); lookAt(s, o, p->los - 5, ll);
            const double da = dopplerHz(137.1e6, la.rangeRateKmS), dt = dopplerHz(137.1e6, lt.rangeRateKmS), dl = dopplerHz(137.1e6, ll.rangeRateKmS);
            CHECK(da > 2500 && da < 3500, "AOS Doppler %.0f Hz", da);
            CHECK(dl < -2500 && dl > -3500, "LOS Doppler %.0f Hz", dl);
            CHECK(std::fabs(dt) < 150, "TCA Doppler %.0f Hz", dt);
            // the range rate is the slope of the range
            Look l0, l1b;
            lookAt(s, o, p->aos + 60 - 0.5, l0); lookAt(s, o, p->aos + 60 + 0.5, l1b);
            Look lm; lookAt(s, o, p->aos + 60, lm);
            CHECK(std::fabs((l1b.rangeKm - l0.rangeKm) - lm.rangeRateKmS) < 0.005, "range rate %.4f vs slope %.4f", lm.rangeRateKmS, l1b.rangeKm - l0.rangeKm);
        }
        // a day of passes from Vilnius: a sun-synchronous satellite comes over 4-12 times above 0 degrees
        Observer vno; vno.latDeg = 54.69; vno.lonDeg = 25.28;
        const double t0 = jdToUnix(t.epochJd);
        const std::vector<Pass> day = findPasses(s, vno, t0, t0 + 86400);
        CHECK(day.size() >= 4 && day.size() <= 12, "%zu passes in a day", day.size());
        for (size_t i = 0; i < day.size(); i++) {
            CHECK(day[i].los > day[i].aos && day[i].tca >= day[i].aos && day[i].tca <= day[i].los && day[i].maxElDeg >= 0, "pass %zu order", i);
            if (i) CHECK(day[i].aos > day[i - 1].los, "passes overlap");
        }
        const std::vector<Pass> high = findPasses(s, vno, t0, t0 + 86400, 20);
        CHECK(high.size() < day.size(), "fewer passes above 20 degrees");
        // the observer's height: from 2 km up the satellite is a little closer and a little lower in the sky (the local horizontal
        // plane rises with the observer), so it rises later and sets earlier
        const Pass* whole = nullptr;   // a pass that is not under way at t0
        for (const auto& q : day) if (!whole && q.aos > t0 + 60) whole = &q;
        CHECK(whole != nullptr, "a whole pass in the day");
        if (whole) {
            Observer hi = vno; hi.altKm = 2.0;
            const Pass& p0 = *whole;
            const double tm = p0.aos + (p0.tca - p0.aos) * 0.3;
            Look lg, lh;
            lookAt(s, vno, tm, lg); lookAt(s, hi, tm, lh);
            CHECK(lh.elDeg < lg.elDeg && lg.elDeg - lh.elDeg < 0.5, "elevation at 2 km %.4f vs %.4f", lh.elDeg, lg.elDeg);
            CHECK(lh.rangeKm < lg.rangeKm && lg.rangeKm - lh.rangeKm < 2.0, "range at 2 km %.3f vs %.3f", lh.rangeKm, lg.rangeKm);
            CHECK(std::fabs(lh.azDeg - lg.azDeg) < 0.05, "azimuth unchanged %.4f vs %.4f", lh.azDeg, lg.azDeg);
            Pass phv; const Pass* ph = nullptr;
            for (const auto& q : findPasses(s, hi, t0, t0 + 86400)) if (!ph && std::fabs(q.tca - p0.tca) < 60) { phv = q; ph = &phv; }
            CHECK(ph != nullptr, "the same pass from 2 km");
            if (ph) {
                CHECK(ph->aos > p0.aos && ph->los < p0.los, "rises later (%.1f s) and sets earlier (%.1f s)", ph->aos - p0.aos, p0.los - ph->los);
                CHECK(ph->maxElDeg < p0.maxElDeg, "lower culmination %.3f vs %.3f", ph->maxElDeg, p0.maxElDeg);
            }
        }
    }
    // ---- when to fetch TLEs again: never fetched -> now; a day after a success; a failed try waits 6 hours; a clock set back does not block
    {
        Observer o;
        o.latDeg = 54.687;
        o.lonDeg = 25.280;
        o.altKm = 0.15;
        Look l;
        CHECK(geoLookAngles(o, 19.2, l), "GEO look Vilnius → 19.2°E");
        CHECK(l.elDeg > 22.0 && l.elDeg < 38.0, "elevation %.2f", l.elDeg);
        CHECK(l.azDeg > 175.0 && l.azDeg < 195.0, "azimuth %.2f", l.azDeg);
        Observer eq;
        eq.latDeg = 0;
        eq.lonDeg = 0;
        CHECK(geoLookAngles(eq, 0.0, l), "GEO look equator under 0° sat");
        CHECK(l.elDeg > 89.0, "zenith el %.2f", l.elDeg);
    }
    {
        const double D = 86400, H = 3600, t = 1.8e9;
        CHECK(tleUpdateDue(t, 0, 0), "never fetched: due");
        CHECK(!tleUpdateDue(t, t - 23 * H, 0), "23 h after a success: not due");
        CHECK(tleUpdateDue(t, t - D, 0) && tleUpdateDue(t, t - 3 * D, 0), "a day after a success: due");
        CHECK(!tleUpdateDue(t, t - 3 * D, t - 1 * H), "old but tried an hour ago: not due");
        CHECK(tleUpdateDue(t, t - 3 * D, t - 7 * H), "old and the last try was 7 h ago: due");
        CHECK(!tleUpdateDue(t, 0, t - 60), "first fetch failed a minute ago: not due");
        CHECK(tleUpdateDue(t, t + 5 * D, 0), "success stamped in the future (clock set back): due");
        CHECK(tleUpdateDue(t, t - 2 * D, t + 100), "a try stamped in the future (clock set back) does not block");
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("passes: all tests passed\n");
    return 0;
}
