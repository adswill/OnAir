// 3.9 smart features, GNSS and science (docs/SMART5-gnss-3.9.md items 21 - 23, docs/SMART5-science-3.9.md items 24 - 26).
// Simulated only: clock drift series with warm-up and outliers, Nakagami-faded intensities, C/N0 with a ground-reflection fringe,
// Sun transits through a Gaussian beam, Poisson meteor counts; no recordings of these were on hand.
#include "dect2/smart5/drift_scan.h"
#include "dect2/smart5/gnss_clock.h"
#include "dect2/smart5/gnss_multipath.h"
#include "dect2/smart5/gnss_s4.h"
#include "dect2/smart5/meteor_rate.h"
#include "dect2/smart5/sun.h"
#include "dect2/smart5/vlf_terminator.h"
#include <cmath>
#include <cstdlib>
#include <random>
#include <vector>
#include "../common/test_util.h"
using namespace dect2::smart5;
static const double kPi = 3.14159265358979323846, kD = kPi / 180;

static void testClock() {
    std::mt19937 rng(10);
    std::normal_distribution<double> n(0, 1);
    for (double ppm : {-25.0, -1.5, 0.4, 3.0, 18.0}) {
        GnssClockCal c;
        double t = 0;
        // warm-up: 0.3 ppm that decays over 10 minutes, fixes once a second, 0.02 ppm noise, a bad fix every 50 s
        for (int i = 0; i < 1800; i++, t++) {
            const double p = ppm + 0.3 * std::exp(-t / 600) + 0.02 * n(rng);
            c.fix(t, (i % 50 == 49 ? p + 5 : p) * 1e-6, true, 6);
            if (i == 120) CHECK(!c.settled(), "settled during warm-up (trend %.4f)", c.trendPpmPerMin());
        }
        CHECK(c.ready() && c.settled() && std::fabs(c.ppm() - ppm) < 0.02, "ppm %.2f got %.3f trend %.4f", ppm, c.ppm(), c.trendPpmPerMin());
        CHECK(std::fabs(c.correctionHz(433.92e6) - 433.92 * c.ppm()) < 1e-6 && !c.text().empty(), "correction");
    }
    GnssClockCal none;
    none.fix(0, 1e-6, false); none.fix(1, 1e-6, true, 3);
    CHECK(!none.ready(), "invalid fixes used");
}

static void testS4() {
    std::mt19937 rng(11);
    for (double m : {1.0, 2.0, 4.0, 11.0, 100.0}) {      // Nakagami-m intensity: S4 = 1 / sqrt(m)
        const double want = 1 / std::sqrt(m);
        for (double cn0 : {45.0, 38.0}) {
            GnssS4 s(50, 60);
            std::gamma_distribution<double> g(m, 1 / m);
            const double c = std::pow(10, cn0 / 10);
            const double sn = std::sqrt(100.0 / c * (1 + 500.0 / (19 * c)));   // noise S4, added independently
            std::normal_distribution<double> noise(0, sn);
            for (int i = 0; i < 3000; i++) {
                const double trend = 1 + 0.2 * std::sin(2 * kPi * i / 3000.0 * 0.2);   // slow elevation change
                s.sample(5, trend * std::max(0.0, g(rng) + noise(rng)), cn0);
            }
            const S4Result r = s.result(5);
            const double tol = 0.05 + 0.12 * want;
            CHECK(r.valid && std::fabs(r.s4 - want) < tol, "m %.0f cn0 %.0f: s4 %.3f want %.3f (raw %.3f noise %.3f)", m, cn0, r.s4, want, r.s4Raw, r.s4Noise);
            if (m <= 2) CHECK(r.level == Scint::Strong || r.level == Scint::Moderate, "level %s", GnssS4::levelName(r.level));
            if (m >= 100) CHECK(r.level == Scint::None, "quiet flagged %s", GnssS4::levelName(r.level));
        }
    }
}

static void testMultipath() {
    std::mt19937 rng(12);
    std::normal_distribution<double> n(0, 0.4);
    const double lambda = 0.1903;
    for (double h : {0.8, 1.5, 3.0}) {
        for (double depth : {0.0, 3.0}) {
            GnssMultipath mp(lambda);
            // a rising pass from 8 to 75 deg, one sample per second over 3 hours
            for (int i = 0; i < 10800; i++) {
                const double el = 8 + 67.0 * i / 10800, s = std::sin(el * kD);
                const double cn0 = 36 + 12 * s + depth / 2 * std::cos(4 * kPi * h * s / lambda) + n(rng);
                mp.sample(7, el, 120, cn0);
            }
            const MultipathResult r = mp.analyse(7);
            CHECK(r.valid, "valid");
            if (depth > 0) CHECK(r.multipath && std::fabs(r.heightM - h) < 0.05 * h + 0.05 && std::fabs(r.depthDb - depth) < 0.6,
                                 "h %.1f: mp %d height %.2f depth %.2f", h, r.multipath, r.heightM, r.depthDb);
            else CHECK(!r.multipath, "h %.1f clean flagged depth %.2f", h, r.depthDb);
        }
    }
}

static void testDriftScan() {
    std::mt19937 rng(13);
    std::normal_distribution<double> n(0, 1);
    const double lat = 54.687, lon = 25.280;
    const double tc = 1774008000;   // 2026-03-20 12:00 UTC
    for (double beam : {0.95, 3.0, 8.0}) {
        for (double lag : {-120.0, 0.0, 200.0}) {
            // the dish is said to point at the Sun's position at tc; it really points where the Sun is at tc + lag
            double daz, del; sunAzEl(tc, lat, lon, daz, del);
            double taz, tel; sunAzEl(tc + lag, lat, lon, taz, tel);
            const double eff = std::sqrt(beam * beam + std::pow(0.589 * 0.533, 2));
            const double rate = 15.0410686 / 3600 * std::cos(sunAt(tc).decDeg * kD);
            const double half = 4 * eff / rate;   // +-4 beamwidths of drift
            std::vector<double> t, p;
            for (double x = tc - half; x <= tc + half; x += 2) {
                double sa, se; sunAzEl(x, lat, lon, sa, se);
                const double c = std::sin(se * kD) * std::sin(tel * kD) + std::cos(se * kD) * std::cos(tel * kD) * std::cos((sa - taz) * kD);
                const double sep = std::acos(std::min(1.0, c)) / kD;
                const double gain = 1 + 0.02 * (x - tc) / half;   // receiver gain drifts 2 %
                t.push_back(x);
                p.push_back(gain * (1 + 9 * std::exp(-4 * std::log(2.0) * sep * sep / (eff * eff))) + 0.05 * n(rng));
            }
            const DriftScanResult r = driftScan(t, p, lat, lon, daz, del, 10.0);
            CHECK(r.valid && std::fabs(r.beamDeg - beam) < 0.04 * beam + 0.02, "beam %.2f got %.3f", beam, r.beamDeg);
            CHECK(std::fabs(r.peakTime - (tc + lag)) < 0.01 * half + 3, "beam %.2f lag %.0f: peak at %+.1f s", beam, lag, r.peakTime - tc);
            CHECK(std::fabs(r.azCorrDeg - (taz - daz)) < 0.02 * beam + 0.01 && std::fabs(r.elCorrDeg - (tel - del)) < 0.02 * beam + 0.01,
                  "corr az %.3f/%.3f el %.3f/%.3f", r.azCorrDeg, taz - daz, r.elCorrDeg, tel - del);
            CHECK(std::fabs(r.yFactorDb - 10) < 0.3 && r.crossErrDeg >= 0 && r.crossErrDeg < 0.15 * beam + 0.05, "Y %.2f cross %.3f", r.yFactorDb, r.crossErrDeg);
        }
    }
    std::vector<double> t(200), p(200, 1.0);
    for (int i = 0; i < 200; i++) t[i] = tc + i;
    CHECK(!driftScan(t, p, lat, lon, 180, 30).valid, "flat trace");
}

static void testTerminator() {
    // NAA Cutler (44.65 N, 67.28 W) to Vilnius, 2026-06-21
    VlfPath path{44.65, -67.28, 54.687, 25.280};
    const double day = 1782000000;
    auto ev = vlfTerminator(path, day);
    int rises = 0, sets = 0, dr = 0;
    for (auto& e : ev) {
        if (e.sunrise) rises++; else sets++;
        if (e.dRegion) dr++;
        double la = path.txLat, lo = path.txLon;
        if (e.at == TermPoint::Midpoint) greatCirclePoint(path.txLat, path.txLon, path.rxLat, path.rxLon, 0.5, la, lo);
        if (e.at == TermPoint::Receiver) { la = path.rxLat; lo = path.rxLon; }
        double az, el; sunAzEl(e.t, la, lo, az, el);
        CHECK(std::fabs(el - (e.dRegion ? -9.3 : -0.833)) < 0.05, "event el %.3f", el);
        CHECK(!termText(e).empty(), "text");
    }
    CHECK(rises == sets && rises >= 5 && dr >= 4, "rises %d sets %d dr %d", rises, sets, dr);
    // Vilnius sunrise on the solstice is about 01:42 UTC (04:42 local), sunset about 19:00 UTC
    bool gotRise = false, gotSet = false;
    for (auto& e : ev)
        if (e.at == TermPoint::Receiver && !e.dRegion) {
            const double h = std::fmod(e.t, 86400) / 3600;
            if (e.sunrise) gotRise = std::fabs(h - (1 + 42 / 60.0)) < 0.1;
            else gotSet = std::fabs(h - 19.0) < 0.15;
        }
    CHECK(gotRise && gotSet, "Vilnius sunrise/sunset");
    // midpoint of a path along the equator
    double la, lo; greatCirclePoint(0, 0, 0, 90, 0.5, la, lo);
    CHECK(std::fabs(la) < 1e-9 && std::fabs(lo - 45) < 1e-9, "midpoint");
    // at local noon in Vilnius in June the whole European half is lit; a mid-Atlantic night leaves part dark
    const double f1 = vlfSunlitFraction(path, day + 12 * 3600), f2 = vlfSunlitFraction(path, day + 3 * 3600 + 1800);
    CHECK(f1 > 0.99 && f2 > 0.05 && f2 < 0.95, "sunlit %.2f %.2f", f1, f2);
}

// Poisson counts by inversion from the raw mt19937 output: std::poisson_distribution is implementation-defined (libc++ on macOS and libstdc++
// on Linux draw different counts from the same engine), which made the outburst check pass on one and fail on the other.
static int poisson(std::mt19937& rng, double mean) {
    const double u = (rng() + 0.5) / 4294967296.0;
    double p = std::exp(-mean), c = p;
    int k = 0;
    while (u > c && k < 10000) { k++; p *= mean / k; c += p; }
    return k;
}

static void testMeteor() {
    std::mt19937 rng(getenv("ONAIR_METEOR_SEED") ? (unsigned)atoi(getenv("ONAIR_METEOR_SEED")) : 14u);   // other seeds for a sweep by hand
    const double lat = 54.687, lon = 25.280;
    // Polaris check of the RA/dec conversion: its elevation is about the latitude
    double az, el; radecAzEl(1765670400, 37.95, 89.264, lat, lon, az, el);
    CHECK(std::fabs(el - lat) < 1.0, "Polaris el %.2f", el);
    MeteorRate mr(lat, lon);
    const double t0 = 1765238400;   // 2025-12-09 00:00 UTC, five days before the Geminids
    const double kTrue = 0.25;
    int falseAlarms = 0;
    bool flagged = false;
    for (int h = 0; h < 24 * 9; h++) {
        const double t = t0 + h * 3600.0;
        double lh = std::fmod(t / 3600 + lon / 15, 24);
        double spor = 20 + 15 * std::cos(2 * kPi * (lh - 6) / 24);
        double sh = 0;
        for (auto& s : showersAt(t + 1800, lat, lon)) if (s.radiantElDeg > 0) sh += s.zhrNow * std::sin(s.radiantElDeg * kD);
        double mean = spor + kTrue * sh;
        const bool inject = h == 24 * 8 + 20;   // an outburst on the last evening
        if (inject) mean *= 3;
        mr.hour(t, poisson(rng, mean));
        if (mr.ready() && mr.hours().back().outburst) { if (inject) flagged = true; else falseAlarms++; }
    }
    CHECK(flagged && falseAlarms <= 1, "outburst %d false %d", flagged, falseAlarms);
    CHECK(std::fabs(mr.sensitivity() - kTrue) < 0.3 * kTrue, "sensitivity %.3f", mr.sensitivity());
    bool gem = false;
    for (auto& s : showersAt(t0 + 5 * 86400 + 2 * 3600, lat, lon)) if (s.name == "Geminids" && s.zhrNow > 100 && s.radiantElDeg > 50) gem = true;
    CHECK(gem, "Geminids at peak");
    CHECK(!mr.text().empty(), "text");
}

int main() {
    testClock(); testS4(); testMultipath(); testDriftScan(); testTerminator(); testMeteor();
    return testSummary("smart5_science");
}
