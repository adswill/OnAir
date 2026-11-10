// 3.9 smart features, satellites (docs/SMART5-sat-3.9.md items 16 - 20). Simulated only: synthetic pass tracks with a blocked
// sector, Sun geometry checked against an independent computation, product and beacon timings with jitter and misses.
#include "dect2/smart5/beacon_learn.h"
#include "dect2/smart5/carrier_log.h"
#include "dect2/smart5/horizon_learn.h"
#include "dect2/smart5/product_cadence.h"
#include "dect2/smart5/sun.h"
#include "dect2/smart5/sun_outage.h"
#include <cmath>
#include <random>
#include <vector>
#include "../common/test_util.h"
using namespace dect2::smart5;
static const double kPi = 3.14159265358979323846, kD = kPi / 180;

// a straight-line pass: azimuth sweeps a0 -> a1, elevation 0 -> maxEl -> 0 over durSec
static std::vector<TrackPoint> pass(double t0, double a0, double a1, double maxEl, double dur) {
    std::vector<TrackPoint> p;
    for (double t = 0; t <= dur; t += 5) {
        const double x = t / dur;
        p.push_back({t0 + t, a0 + (a1 - a0) * x, maxEl * std::sin(kPi * x)});
    }
    return p;
}

static void testHorizon() {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(0, 1);
    // real horizon: 25 deg towards the east (sectors 60 - 150 deg, trees), 5 deg elsewhere
    auto realH = [](double az) { return az >= 60 && az < 150 ? 25.0 : 5.0; };
    HorizonLearn h;
    for (int k = 0; k < 40; k++) {
        const double a0 = std::fmod(k * 47.0, 360), a1 = a0 + 160;
        for (auto& q : pass(0, a0, a1, 20 + 60 * u(rng), 900)) {
            const double az = std::fmod(q.azDeg, 360);
            const bool dec = q.elDeg >= realH(az) + 2 * (u(rng) - 0.5) && u(rng) > 0.05;   // 5 % random frame loss
            h.sample(az, q.elDeg, dec);
        }
    }
    CHECK(std::fabs(h.horizon(100) - 25) <= 3 && std::fabs(h.horizon(250) - 5) <= 3, "horizon E %.0f W %.0f", h.horizon(100), h.horizon(250));
    // two passes of the same peak elevation: one over the trees, one over open sky; the open one decodes longer
    std::vector<std::vector<TrackPoint>> next = {pass(0, 30, 170, 35, 800), pass(0, 200, 340, 35, 800), pass(0, 200, 340, 12, 600)};
    auto b = h.best(next);
    CHECK(b.index == 1 && h.predictMinutes(next[0]) < h.predictMinutes(next[1]), "best %d %.1f vs %.1f", b.index, h.predictMinutes(next[0]), h.predictMinutes(next[1]));
    HorizonLearn c; c.load(h.save());
    CHECK(c.horizon(100) == h.horizon(100) && c.horizon(250) == h.horizon(250), "save/load");
    HorizonLearn fresh(10);
    CHECK(fresh.horizon(0) == 10 && !fresh.learned(0), "default");
}

static void testSun() {
    // 2026-03-20 14:46 UTC (March equinox): declination near 0
    const double eq = 1774018000;
    CHECK(std::fabs(sunAt(eq).decDeg) < 0.1, "equinox dec %.3f", sunAt(eq).decDeg);
    // 2026-06-21 08:24 UTC (June solstice): declination 23.44
    CHECK(std::fabs(sunAt(1782030240).decDeg - 23.44) < 0.05, "solstice dec %.3f", sunAt(1782030240).decDeg);
    // local noon at Greenwich (2026-06-21 12:01:40 UTC, equation of time -1.7 min): azimuth 180, elevation 90 - 51.48 + 23.44
    double az, el; sunAzEl(1782043300, 51.48, 0, az, el);
    CHECK(std::fabs(az - 180) < 1.0 && std::fabs(el - (90 - 51.48 + 23.44)) < 0.1, "noon az %.2f el %.2f", az, el);
}

static void testSunOutage() {
    CHECK(std::fabs(dishBeamwidthDeg(0.8, 11.7e9) - 70 * 0.02562 / 0.8) < 0.01, "beamwidth");
    // on the equator under the satellite the outage is at local noon on the equinox days, one per day, a few minutes
    const double t0 = 1773100800;   // 2026-03-10
    auto v = sunOutages(t0, t0 + 25 * 86400.0, 0, 30, 30, 2.0);
    CHECK(v.size() >= 3 && v.size() <= 9, "equator outages %zu", v.size());
    for (auto& o : v) {
        const double dec = sunAt(o.peak).decDeg;
        double tod = std::fmod(o.peak, 86400) / 3600 + 30 / 15.0;   // local mean time
        CHECK(std::fabs(dec) < 1.3 && std::fabs(tod - 12) < 0.3 && o.end - o.start < 900, "dec %.2f tod %.2f len %.0f", dec, tod, o.end - o.start);
    }
    // Vilnius looking at 9 E (Eutelsat 9B), 0.8 m dish at 11.7 GHz: the Sun must sit at the satellite's topocentric declination
    const double lat = 54.687, lon = 25.280, sl = 9.0, bw = dishBeamwidthDeg(0.8, 11.7e9);
    double site[3]; siteEcef(lat, lon, 0, site);
    const double sat[3] = {42164.17 * std::cos(sl * kD), 42164.17 * std::sin(sl * kD), 0};
    const double vv[3] = {sat[0] - site[0], sat[1] - site[1], sat[2] - site[2]};
    const double topoDec = std::asin(vv[2] / std::sqrt(vv[0] * vv[0] + vv[1] * vv[1] + vv[2] * vv[2])) / kD;
    double saz, sel; dirAzEl(vv, lat, lon, saz, sel);
    const double y0 = 1767225600;   // 2026-01-01
    auto w = sunOutages(y0, y0 + 365 * 86400.0, lat, lon, sl, bw);
    int spring = 0, autumn = 0;
    for (auto& o : w) {
        double az, el; sunAzEl(o.peak, lat, lon, az, el);
        const double dec = sunAt(o.peak).decDeg;
        CHECK(std::fabs(dec - topoDec) < bw / 2 + 0.6 && std::fabs(az - saz) < 2 && std::fabs(el - sel) < 1.5, "dec %.2f want %.2f az %.1f/%.1f el %.1f/%.1f", dec, topoDec, az, saz, el, sel);
        const double doy = (o.peak - y0) / 86400;
        if (doy < 182) spring++; else autumn++;
    }
    CHECK(spring >= 2 && autumn >= 2, "season counts %d %d (topo dec %.2f)", spring, autumn, topoDec);
    if (!w.empty()) {
        CHECK(inSunOutage(w[0].peak, lat, lon, sl, bw) && !inSunOutage(w[0].peak + 7200, lat, lon, sl, bw), "inSunOutage");
        CHECK(!sunOutageText(w[0]).empty(), "text");
    }
}

static void testCadence() {
    std::mt19937 rng(8);
    std::normal_distribution<double> j(0, 8);
    ProductCadence c;
    double t = 0;
    // GK-2A full disk every 600 s with 8 s jitter; product 7 is not sent (lock held); later lock is lost around product 14
    for (int s = 0; s < 600 * 20; s++, t++) {
        const bool lost = t > 600 * 14 - 200 && t < 600 * 14 + 400;
        c.link(t, !lost, !lost);
        if (s % 600 == 300) {
            const int k = s / 600;
            if (k != 7 && !lost) c.product("FD", t + j(rng));
        }
        c.tick(t);
        if (s == 600 * 7 + 300 + 400) {
            auto st = c.states();
            CHECK(st.size() == 1 && st[0].missing == MissCause::NotSent && std::fabs(st[0].periodSec - 600) < 5, "not sent: %d %.1f", st.empty() ? -1 : (int)st[0].missing, st.empty() ? 0 : st[0].periodSec);
            CHECK(c.text("FD").find("did not send") != std::string::npos, "text %s", c.text("FD").c_str());
        }
        if (s == 600 * 14 + 300 + 400) {
            auto st = c.states();
            CHECK(st[0].missing == MissCause::LostLock, "lost lock %d", (int)st[0].missing);
        }
    }
    auto st = c.states();
    CHECK(st[0].missing == MissCause::None && st[0].missedCount == 2, "end missed %d", st[0].missedCount);
}

static void testCarrierLog() {
    CarrierLog l;
    const double f = 1545.0e6;
    double t = 0;
    auto run = [&](int sec, double snr, int fr, double el, bool sun) { for (int i = 0; i < sec; i++, t++) l.sample(f, t, {snr, fr, el, sun}); };
    run(600, 12, 2, 40, false);
    run(300, 3, 0, 40, true);    // sun outage
    run(600, 12, 2, 40, false);
    run(120, 11.5, 0, 40, false);   // blocked, SNR the same: unexplained
    run(300, 12, 2, 40, false);
    run(200, 2, 0, 6, false);    // satellite set
    run(60, 12, 2, 40, false);
    auto o = l.outages(f);
    CHECK(o.size() == 3 && o[0].reason == OutageReason::SunOutage && o[1].reason == OutageReason::Unexplained && o[2].reason == OutageReason::LowElevation,
          "outages %zu %d %d %d", o.size(), o.size() > 0 ? (int)o[0].reason : -1, o.size() > 1 ? (int)o[1].reason : -1, o.size() > 2 ? (int)o[2].reason : -1);
    CHECK(o.size() == 3 && std::fabs(o[0].end - o[0].start - 300) < 3, "length");
    const double av = l.availabilityPct(f);
    CHECK(av > 70 && av < 85 && l.minutes(f).size() >= 35, "availability %.1f", av);
}

static void testBeacon() {
    std::mt19937 rng(9);
    std::normal_distribution<double> j(0, 0.3);
    std::uniform_real_distribution<double> u(0, 1);
    for (double P : {10.0, 30.0, 60.0, 117.0}) {
        BeaconLearn b;
        double t = 1000;
        // three passes of 10 minutes, 40 % of the beacons lost, 95 min apart
        for (int pass = 0; pass < 3; pass++) {
            const double start = t + pass * 5700;
            for (double x = start; x < start + 600; x += P) if (u(rng) > 0.4) b.beacon(42017, x + j(rng));
        }
        const BeaconState s = b.state(42017);
        CHECK(std::fabs(s.periodSec - P) < 0.02 * P && s.jitterSec < 1.0, "P %.0f learned %.2f jitter %.2f", P, s.periodSec, s.jitterSec);
        // during the next pass the satellite goes silent: one overdue beacon is flagged per period while up
        const double start = t + 3 * 5700;
        b.beacon(42017, start);
        for (double x = start; x < start + 3.5 * P; x += 1) b.tick(42017, x, true);
        CHECK(b.state(42017).overdue && b.state(42017).missedWhileUp == 3, "P %.0f missed %d", P, b.state(42017).missedWhileUp);
        // below the horizon nothing is counted
        for (double x = start + 3.5 * P; x < start + 20 * P; x += 1) b.tick(42017, x, false);
        CHECK(b.state(42017).missedWhileUp == 3 && !b.state(42017).overdue, "down counted");
        CHECK(!b.text(42017, "sat", start).empty(), "text");
    }
}

int main() {
    testHorizon(); testSun(); testSunOutage(); testCadence(); testCarrierLog(); testBeacon();
    return testSummary("smart5_sat");
}
