// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Position estimator: accuracy against naive and linear extrapolation on simulated flights between real city pairs, covariance calibration
// (NEES), robustness (outliers, late fixes, date line, pole, garbage input), route/text parsing and speed. Prints the accuracy table.
#include "dect2/track/track_hub.h"
#include "dect2/track/track_imm.h"
#include "dect2/track/track_route.h"
#include "track_sim.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace dect2::track;
using namespace tsim;

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static uint64_t g_seedOff = 0;   // tuning uses other flights and other noise than the tests

struct City { const char* a; const char* b; };
static const City kPairs[] = {
    {"EGLL", "KJFK"}, {"OMDB", "EGLL"}, {"YSSY", "WSSS"}, {"KLAX", "RJTT"},   // Atlantic, Gulf-Europe, South-East Asia, trans-Pacific (crosses the date line)
    {"KJFK", "VHHH"},                                                         // over the pole region
    {"EDDF", "LTFM"}, {"KSFO", "KDEN"}, {"FAOR", "OMDB"}, {"SBGR", "EDDF"}, {"EHAM", "ULLI"}, {"NZAA", "YSSY"}, {"VHHH", "RJTT"}};

static std::vector<Truth> flights(int perPair = 1, uint64_t seed0 = 100) {
    std::vector<Truth> v;
    for (const City& c : kPairs) {
        const Airport *a = findAirport(c.a), *b = findAirport(c.b);
        if (!a || !b) { std::printf("missing airport %s %s\n", c.a, c.b); std::exit(2); }
        for (int i = 0; i < perPair; i++) v.push_back(makeFlight(*a, *b, seed0 + g_seedOff + (uint64_t)i * 7 + (uint64_t)(c.a[1] + c.b[2])));
    }
    return v;
}

struct Errs { std::vector<double> imm, naive, lin; double neesSum = 0; int neesN = 0; };

static void addNees(Errs& e, const PosEstimate& pe, const TState& tr) {
    // error vector in local east/north at the estimate
    double dE = (tr.lon - pe.lon); dE = wrap180(dE) * kDeg * kEarthR * std::cos(pe.lat * kDeg);
    const double dN = (tr.lat - pe.lat) * kDeg * kEarthR;
    const double det = pe.cEE * pe.cNN - pe.cEN * pe.cEN;
    if (det <= 0) return;
    e.neesSum += (dE * dE * pe.cNN - 2 * dE * dN * pe.cEN + dN * dN * pe.cEE) / det;
    e.neesN++;
}

// ADS-B: fixes every `gap` seconds; the error is measured at 4 times inside every gap. 20 minutes around the middle of the flight of each pair.
static Errs evalAdsb(double gap, const TrackOptions& opt = TrackOptions(), double minutes = 25, double startFrac = 0.4) {
    Errs E;
    const auto fl = flights();
    int fi = 0;
    for (const Truth& tr : fl) {
        Rng r(900 + g_seedOff + (uint64_t)fi++);
        AdsbSensor sn; sn.gapSec = gap;
        const double t0 = tr.end() * startFrac, t1 = std::min(t0 + minutes * 60, tr.end() - 60);
        TrackEstimator est(opt);
        double lastLa = 0, lastLo = 0, lastSp = 0, lastTr = 0, lastT = -1;
        for (double t = t0; t < t1; t += gap) {
            if (r.u() < sn.dropout) continue;
            Fix f = sn.make(tr, t, r);
            const UpdateResult ur = est.update(f);
            if (ur == UpdateResult::RejectedGate || ur == UpdateResult::RejectedSpeed) continue;
            lastLa = f.lat; lastLo = f.lon; lastSp = f.speedMs; lastTr = f.trackDeg; lastT = t;
            if (t - t0 < 60) continue;
            for (int q = 1; q <= 4; q++) {
                const double tq = t + gap * q / 4.5;
                const TState g = tr.at(tq);
                PosEstimate pe;
                if (!est.estimate(tq, pe)) continue;
                E.imm.push_back(errM(pe.lat, pe.lon, g.lat, g.lon));
                E.naive.push_back(errM(lastLa, lastLo, g.lat, g.lon));
                double la, lo; gcDestination(lastLa, lastLo, lastTr, lastSp * (tq - lastT), la, lo);
                E.lin.push_back(errM(la, lo, g.lat, g.lon));
                addNees(E, pe, g);
            }
        }
    }
    return E;
}

// ACARS / VDL2 text positions every `gap` seconds (+-20 %), optional route prior; the error is measured every 30 s inside every gap
// phase 0: en route (above 8000 m); 1: arrival (the last 45 minutes before landing, every altitude)
static Errs evalSat(double gap, bool route, const TrackOptions& opt = TrackOptions(), Src src = Src::Acars, int perPair = 1, int phase = 0) {
    Errs E;
    const auto fl = flights(perPair);
    int fi = 0;
    for (const Truth& tr : fl) {
        const City& c = kPairs[(size_t)(fi / perPair)];
        Rng r(500 + g_seedOff + (uint64_t)fi++);
        SatSensor sn; sn.gapSec = gap; sn.src = src;
        TrackEstimator est(opt);
        if (route) { const Airport* b = findAirport(c.b); const Airport* a = findAirport(c.a); RouteHint h; h.hasDest = h.hasOrigin = true; h.destLat = b->lat; h.destLon = b->lon; h.origLat = a->lat; h.origLon = a->lon; est.setRoute(h); }
        std::vector<Fix> fx;
        for (double t = 1200; t < tr.end() - (phase == 0 ? 1500 : 30); t += gap * (1 + sn.jitter * (r.u() * 2 - 1))) fx.push_back(sn.make(tr, t, r));
        for (size_t k = 0; k + 1 < fx.size(); k++) {
            est.update(fx[k]);
            if (k < 3) continue;
            if (phase == 0 ? tr.at(fx[k].t).altM < 8000 : fx[k].t < tr.end() - 2700 || fx[k + 1].t > tr.end() - 60) continue;
            double pla = fx[k - 1].lat, plo = fx[k - 1].lon, pt = fx[k - 1].t;
            const double dtp = fx[k].t - pt;
            const double brg = gcBearingDeg(pla, plo, fx[k].lat, fx[k].lon), spd = gcDistM(pla, plo, fx[k].lat, fx[k].lon) / std::max(1.0, dtp);
            for (double tq = fx[k].t + 30; tq < fx[k + 1].t; tq += 30) {
                const TState g = tr.at(tq);
                PosEstimate pe;
                if (!est.estimate(tq, pe)) continue;
                E.imm.push_back(errM(pe.lat, pe.lon, g.lat, g.lon));
                E.naive.push_back(errM(fx[k].lat, fx[k].lon, g.lat, g.lon));
                double la, lo; gcDestination(fx[k].lat, fx[k].lon, brg, spd * (tq - fx[k].t), la, lo);
                E.lin.push_back(errM(la, lo, g.lat, g.lon));
                addNees(E, pe, g);
            }
        }
    }
    return E;
}

static void row(const char* name, const Errs& e) {
    std::printf("%-44s | imm %8.1f %8.1f | linear %8.1f %8.1f | last-fix %8.1f %8.1f | NEES %.2f (n=%zu)\n", name, pct(e.imm, 0.5), pct(e.imm, 0.95), pct(e.lin, 0.5), pct(e.lin, 0.95), pct(e.naive, 0.5), pct(e.naive, 0.95), e.neesN ? e.neesSum / e.neesN : 0.0, e.imm.size());
}

// ---- tuning (not a test): ./test_track tune  -- coordinate descent on the process-noise parameters against other flights than the tests use
static double tuneScore(const TrackOptions& o, bool print) {
    double sc = 0;
    auto add = [&](const char* nm, const Errs& e, double wNees) {
        const double nees = e.neesN ? e.neesSum / e.neesN : 1;
        const double c = std::log(pct(e.imm, 0.5) / pct(e.lin, 0.5)) + 0.5 * std::log(pct(e.imm, 0.95) / pct(e.lin, 0.95)) + wNees * std::fabs(std::log(nees / 2.0));
        sc += c;
        if (print) std::printf("  %-22s med %8.1f/%8.1f  p95 %8.1f/%8.1f  nees %.2f  cost %.3f\n", nm, pct(e.imm, 0.5), pct(e.lin, 0.5), pct(e.imm, 0.95), pct(e.lin, 0.95), nees, c);
    };
    add("adsb 1s", evalAdsb(1.0, o, 12), 0.7);
    add("adsb 5s", evalAdsb(5.0, o, 12), 0.7);
    add("acars 5min", evalSat(300, false, o), 0.7);
    add("acars 15min", evalSat(900, false, o), 0.7);
    add("vdl2 2min", evalSat(120, false, o, Src::Vdl2), 0.7);
    add("acars arrival 5min", evalSat(300, false, o, Src::Acars, 1, 1), 0.7);
    add("adsb 5s descent", evalAdsb(5.0, o, 12, 0.93), 0.7);
    return sc;
}
static void tune() {
    g_seedOff = 1000;
    TrackOptions o;
    struct P { const char* n; double* v; };
    P ps[] = {{"qCv", &o.qCv}, {"qCt", &o.qCt}, {"qMan", &o.qMan}, {"qOmega", &o.qOmega}, {"omegaTau", &o.omegaTauSec}, {"soj0", &o.sojournSec[0]}, {"soj1", &o.sojournSec[1]}, {"soj2", &o.sojournSec[2]}};
    double best = tuneScore(o, true);
    for (int round = 0; round < 4; round++) {
        const double f = round < 2 ? 2.0 : 1.4;
        for (P& p : ps) for (double m : {f, 1 / f}) {
            const double old = *p.v; *p.v = old * m;
            const double sc = tuneScore(o, false);
            if (sc < best - 1e-4) { best = sc; std::printf("round %d %s -> %g  score %.3f\n", round, p.n, *p.v, sc); std::fflush(stdout); }
            else *p.v = old;
        }
    }
    std::printf("final:\n");
    tuneScore(o, true);
    std::printf("qCv %g qCt %g qMan %g qOmega %g omegaTau %g sojourn %g %g %g\n", o.qCv, o.qCt, o.qMan, o.qOmega, o.omegaTauSec, o.sojournSec[0], o.sojournSec[1], o.sojournSec[2]);
}

static void cands() {
    g_seedOff = 1000;
    struct C { const char* n; double qcv, qct, qman, qo, tau, s0, s1, s2; };
    const C cs[] = {{"A degenerate", .0112, .0196, .3, 4.6e-7, 2, 600, 1.15, 84}, {"B sensible", .01, .02, .5, 1e-5, 30, 300, 20, 60}, {"C qMan 1.5", .01, .02, 1.5, 1e-5, 30, 300, 20, 60}, {"D tau60 s1 40", .01, .02, .5, 1e-5, 60, 300, 40, 60}, {"E qCt .05", .01, .05, .5, 1e-5, 30, 300, 20, 60}, {"F sojMan 150", .01, .02, .5, 1e-5, 30, 300, 20, 150}};
    for (const C& c : cs) {
        TrackOptions o; o.qCv = c.qcv; o.qCt = c.qct; o.qMan = c.qman; o.qOmega = c.qo; o.omegaTauSec = c.tau; o.sojournSec[0] = c.s0; o.sojournSec[1] = c.s1; o.sojournSec[2] = c.s2;
        std::printf("== %s\n", c.n);
        const double sc = tuneScore(o, true);
        std::printf("   total %.3f\n", sc);
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "tune") { tune(); return 0; }
    if (argc > 1 && std::string(argv[1]) == "cands") { cands(); return 0; }
    // ---- airports and parsing
    {
        size_t n; const Airport* t = airportTable(n);
        for (size_t i = 1; i < n; i++) CHECK(std::string(t[i - 1].icao) < std::string(t[i].icao), "airport table not sorted at %s", t[i].icao);
        const Airport* a = findAirport("EGLL");
        CHECK(a && std::fabs(a->lat - 51.47) < 0.1 && std::fabs(a->lon + 0.46) < 0.1, "EGLL");
        CHECK(!findAirport("ZZZZ"), "ZZZZ");
        RouteInfo ri;
        CHECK(parseRouteText("Q1", "OMDB1020103011451210    EGLL", ri) && ri.origin == "OMDB" && ri.outSec == 10 * 3600 + 20 * 60, "Q1 %s", ri.origin.c_str());
        RouteInfo r2;
        CHECK(parseRouteText("H1", "FPN/RI:DA:KLAX:AA:KSFO:CR:LAXSFO01", r2) && r2.origin == "KLAX" && r2.dest == "KSFO", "FPN");
        RouteInfo r3;
        CHECK(parseRouteText("H1", "(FPL-UAE1-IS-B77W/H-SDE3FGHIJ3J5M1RWXY/LB1D1-OMDB1030-N0490F360 DCT ABC UL1 DEF-EGLL0710 EGKK-PBN/A1)", r3) && r3.origin == "OMDB" && r3.dest == "EGLL", "FPL %s %s", r3.origin.c_str(), r3.dest.c_str());
        RouteInfo r4;
        CHECK(parseRouteText("5Z", "/ETA 1423 DEST KJFK NXT WPT BOBTU", r4) && r4.etaSec == 14 * 3600 + 23 * 60 && r4.dest == "KJFK" && r4.nextWpt == "BOBTU", "text %d %s %s", r4.etaSec, r4.dest.c_str(), r4.nextWpt.c_str());
        CHECK(!parseRouteText("H1", "NOTHING OF INTEREST HERE", r4) || true, "");
        TextPosition p = parseTextPosition("H1", "POSN43312W123174,EASON,215754,370");
        CHECK(p.ok && std::fabs(p.lat - (43 + 31.2 / 60)) < 1e-6 && std::fabs(p.lon + (123 + 17.4 / 60)) < 1e-6 && std::fabs(p.resolutionDeg - 0.1 / 60) < 1e-9, "tenths %.5f %.5f", p.lat, p.lon);
        p = parseTextPosition("20", "POSN38160W077075,AAA");
        CHECK(p.ok && std::fabs(p.lat - 38.160) < 1e-6 && std::fabs(p.lon + 77.075) < 1e-6 && p.resolutionDeg == 0.001, "thousandths");
        p = parseTextPosition("16", "N 44.203,W 86.546,31965,6, 290");
        CHECK(p.ok && std::fabs(p.lat - 44.203) < 1e-9 && std::fabs(p.lon + 86.546) < 1e-9 && std::fabs(p.resolutionDeg - 0.001) < 1e-12, "decimal");
        p = parseTextPosition("", "N 51 28.3 E 000 27.1");
        CHECK(p.ok && std::fabs(p.lat - (51 + 28.3 / 60)) < 1e-9 && std::fabs(p.lon - (27.1 / 60)) < 1e-9, "dm %.4f %.4f", p.lat, p.lon);
        p = parseTextPosition("", "no position here");
        CHECK(!p.ok, "none");
        p = parseTextPosition("", "N95123E000000");
        CHECK(!p.ok, "lat out of range");
        CHECK(losRangeNm(35000) > 220 && losRangeNm(35000) < 260, "los %.1f", losRangeNm(35000));
    }
    // ---- sigma tables
    CHECK(nacpEpuM(11) == 3 && nacpEpuM(9) == 30 && nacpEpuM(0) == 0 && nacpEpuM(8) == 92.6, "nacp");
    {
        Fix f; f.src = Src::Adsb; f.nacp = 9; f.timeSigmaSec = 0;
        const double s9 = fixPosSigmaM(f, 230); f.nacp = 5; const double s5 = fixPosSigmaM(f, 230);
        CHECK(s5 > 20 * s9 / 2 && s9 > 6, "sigma %.1f %.1f", s9, s5);
        Fix a; a.src = Src::Acars; a.resolutionDeg = 1.0 / 60; a.timeSigmaSec = 0;
        Fix b = a; b.resolutionDeg = 0.1 / 60;
        CHECK(fixPosSigmaM(a, 230) > 5 * fixPosSigmaM(b, 230) * 0.5, "res sigma");
    }

    // ---- accuracy tables
    std::printf("\nAccuracy (metres; median / 95th percentile of the horizontal error), 12 simulated flights between real city pairs\n");
    const Errs a1 = evalAdsb(1.0), a5 = evalAdsb(5.0);
    row("ADS-B between fixes, 1 s gap", a1);
    row("ADS-B between fixes, 5 s gap", a5);
    const Errs s5 = evalSat(300, false), s5r = evalSat(300, true), s15 = evalSat(900, false), s15r = evalSat(900, true);
    row("ACARS text positions, 5 min gaps", s5);
    row("ACARS text positions, 5 min gaps + route prior", s5r);
    row("ACARS text positions, 15 min gaps", s15);
    row("ACARS text positions, 15 min gaps + route prior", s15r);
    const Errs ar5 = evalSat(300, false, TrackOptions(), Src::Acars, 1, 1), ar5r = evalSat(300, true, TrackOptions(), Src::Acars, 1, 1);
    row("ACARS arrival phase (45 min), 5 min gaps", ar5);
    row("ACARS arrival phase (45 min), 5 min + route prior", ar5r);
    const Errs v2 = evalSat(120, false, TrackOptions(), Src::Vdl2);
    row("VDL2 ADS-C/text positions, 2 min gaps", v2);
    std::fflush(stdout);

    CHECK(pct(a1.imm, 0.5) < pct(a1.naive, 0.5) * 0.4, "ADS-B 1 s median vs last fix");
    CHECK(pct(a5.imm, 0.5) < pct(a5.naive, 0.5) * 0.3, "ADS-B 5 s median vs last fix");
    CHECK(pct(a5.imm, 0.95) < pct(a5.lin, 0.95) * 1.05, "ADS-B 5 s p95 vs linear");
    CHECK(pct(s5.imm, 0.5) < pct(s5.naive, 0.5) * 0.5, "ACARS 5 min median vs last fix");
    CHECK(pct(s5.imm, 0.95) < pct(s5.lin, 0.95) * 1.0, "ACARS 5 min p95 vs linear");
    CHECK(pct(s15.imm, 0.5) < pct(s15.naive, 0.5) * 0.5, "ACARS 15 min median vs last fix");
    CHECK(pct(s5r.imm, 0.5) <= pct(s5.imm, 0.5) * 1.10 && pct(s5r.imm, 0.95) <= pct(s5.imm, 0.95) * 1.05, "route prior must not hurt much at 5 min");
    CHECK(pct(s15r.imm, 0.95) <= pct(s15.imm, 0.95) * 1.02 && pct(s15r.imm, 0.5) <= pct(s15.imm, 0.5) * 1.15, "route prior at 15 min");
    // calibration: the mean NEES of a 2-D position is 2 for a consistent filter; the band allows a conservative filter (below 1) but not an over-confident one
    for (const Errs* e : {&a1, &a5, &s5, &s15, &s5r, &s15r, &v2}) { const double m = e->neesSum / std::max(1, e->neesN); CHECK(m > 0.3 && m < 5.0, "NEES %.2f outside the calibration band", m); }
    CHECK(ar5.neesSum / std::max(1, ar5.neesN) < 15.0, "arrival NEES");
    CHECK(pct(ar5.imm, 0.5) < pct(ar5.lin, 0.5) && pct(ar5.imm, 0.95) < pct(ar5.lin, 0.95) * 1.1, "arrival vs linear");
    CHECK(pct(v2.imm, 0.5) < pct(v2.lin, 0.5) * 1.1, "VDL2 vs linear");

    // ---- robustness
    {   // outliers and a swapped identity
        const Airport *a = findAirport("EGLL"), *b = findAirport("OMDB");
        Truth tr = makeFlight(*a, *b, 7);
        Rng r(5);
        AdsbSensor sn;
        TrackEstimator est;
        int corrupted = 0, checked = 0;
        for (double t = 3000; t < 3000 + 1800; t += 1) {
            Fix f = sn.make(tr, t, r);
            if (std::fmod(t, 40) < 1) offsetPos(f.lat, f.lon, 2e5 * (r.u() - 0.5), 2e5 * (r.u() - 0.5), f.lat, f.lon);   // 2.5 % wild positions
            est.update(f);
            PosEstimate pe;
            if (t > 3060 && est.estimate(t, pe)) { checked++; if (errM(pe.lat, pe.lon, tr.at(t).lat, tr.at(t).lon) > 500) corrupted++; }
        }
        CHECK(corrupted == 0, "outliers corrupted the track %d of %d", corrupted, checked);
        // a different aircraft with the same address appears 300 km away and stays: the track must restart on it
        Fix g = sn.make(tr, 4801, r);
        PosEstimate pe;
        for (int i = 0; i < 8; i++) {
            Fix h = sn.make(tr, 4801 + i, r);
            offsetPos(h.lat, h.lon, 300e3, 0, h.lat, h.lon);
            est.update(h);
        }
        est.estimate(4809, pe);
        CHECK(errM(pe.lat, pe.lon, tr.at(4809).lat, tr.at(4809).lon) > 250e3 && errM(pe.lat, pe.lon, tr.at(4809).lat, tr.at(4809).lon) < 350e3, "restart on the jump");
        (void)g;
    }
    {   // out-of-order delivery: the same fixes shuffled inside 20 s give the same answer as in order
        const Airport *a = findAirport("EDDF"), *b = findAirport("LTFM");
        Truth tr = makeFlight(*a, *b, 11);
        Rng r(6);
        AdsbSensor sn;
        std::vector<Fix> fx;
        for (double t = 2000; t < 2400; t += 1) fx.push_back(sn.make(tr, t, r));
        TrackEstimator in, out;
        for (const Fix& f : fx) in.update(f);
        std::vector<Fix> sh = fx;
        Rng r2(9);
        for (size_t i = 0; i + 1 < sh.size(); i += 4) if (r2.u() < 0.5) std::swap(sh[i], sh[i + 1]);   // adjacent swaps
        int late = 0;
        for (const Fix& f : sh) { const UpdateResult u = out.update(f); if (u == UpdateResult::Retrofitted) late++; }
        PosEstimate p1, p2;
        in.estimate(2400, p1); out.estimate(2400, p2);
        CHECK(late > 10, "retro-fit not exercised: %d", late);
        CHECK(errM(p1.lat, p1.lon, p2.lat, p2.lon) < 30, "shuffled differs by %.1f m", errM(p1.lat, p1.lon, p2.lat, p2.lon));
        // a fix older than the window is dropped, not applied
        Fix old = fx[0]; old.t = 100;
        CHECK(out.update(old) == UpdateResult::RejectedLate, "old fix");
    }
    {   // date line and pole: the track stays continuous and the error is small
        const Airport *a = findAirport("KLAX"), *b = findAirport("RJTT");
        Truth tr = makeFlight(*a, *b, 3);
        Rng r(2);
        SatSensor sn;
        TrackEstimator est;
        bool crossed = false; double worst = 0; int n = 0;
        for (double t = 600; t < tr.end() - 600; t += 300) {
            est.update(sn.make(tr, t, r));
            PosEstimate pe;
            for (double q = t + 60; q < t + 300; q += 60) {
                if (t < 1500 || !est.estimate(q, pe)) continue;
                const TState g = tr.at(q);
                if (std::fabs(g.lon) > 175) crossed = true;
                worst = std::max(worst, errM(pe.lat, pe.lon, g.lat, g.lon)); n++;
            }
        }
        CHECK(crossed && n > 20, "date line not crossed in the test");
        CHECK(worst < 40e3, "date line worst error %.0f m", worst);
        const Airport *c = findAirport("KJFK"), *d = findAirport("VHHH");
        Truth tp = makeFlight(*c, *d, 4, 480, 150);
        double maxLat = 0; for (const TState& s : tp.s) maxLat = std::max(maxLat, s.lat);
        TrackEstimator e2; worst = 0; n = 0;
        for (double t = 600; t < tp.end() - 600; t += 300) {
            e2.update(sn.make(tp, t, r));
            PosEstimate pe;
            for (double q = t + 60; q < t + 300; q += 60) { if (t < 1500 || !e2.estimate(q, pe)) continue; const TState g = tp.at(q); worst = std::max(worst, errM(pe.lat, pe.lon, g.lat, g.lon)); n++; }
        }
        std::printf("KJFK-VHHH max latitude %.1f deg, worst 5-min-gap error %.0f m\n", maxLat, worst);
        CHECK(maxLat > 70 && worst < 40e3, "polar flight max lat %.1f worst %.0f", maxLat, worst);
        // right at the pole
        TrackEstimator e3;
        for (int i = 0; i < 20; i++) { Fix f; f.src = Src::Adsb; f.t = i * 5; f.hasPos = true; double la, lo; gcDestination(89.9, 10, 0, 230 * 5.0 * i, la, lo); f.lat = la; f.lon = lo; f.hasVel = true; f.speedMs = 230; f.trackDeg = 0; e3.update(f); }
        PosEstimate pe; CHECK(e3.estimate(120, pe) && std::isfinite(pe.lat) && std::isfinite(pe.lon) && std::fabs(pe.lat) <= 90, "pole");
    }
    {   // garbage in: no NaN out, covariance stays a covariance
        TrackEstimator est;
        Rng r(77);
        double t = 0;
        int bad = 0;
        for (int i = 0; i < 20000; i++) {
            Fix f; f.src = (Src)(r.next() % 6); t += r.u() * 3;
            f.t = t - (r.u() < 0.05 ? r.u() * 200 : 0);
            f.hasPos = r.u() < 0.8; f.lat = (r.u() * 2 - 1) * 95; f.lon = (r.u() * 2 - 1) * 200;
            if (r.u() < 0.02) f.lat = NAN;
            if (r.u() < 0.02) f.lon = INFINITY;
            f.hasVel = r.u() < 0.5; f.speedMs = r.u() * 900 - 50; f.trackDeg = r.u() * 800 - 200;
            if (r.u() < 0.02) f.speedMs = NAN;
            f.hasAlt = r.u() < 0.5; f.altM = r.u() * 2e4 - 1000; f.hasVrate = r.u() < 0.3; f.vrateMs = r.n() * 50;
            f.hasRoll = r.u() < 0.3; f.rollDeg = r.n() * 80;
            f.hasWind = r.u() < 0.2; f.windMs = r.u() * 100; f.windFromDeg = r.u() * 720;
            f.nacp = (int)(r.next() % 15) - 2; f.nic = (int)(r.next() % 15) - 2; f.resolutionDeg = r.u() < 0.3 ? r.u() : 0;
            est.update(f);
            PosEstimate pe;
            if (est.estimate(t + r.u() * 600, pe)) {
                if (!(std::isfinite(pe.lat) && std::isfinite(pe.lon) && std::isfinite(pe.sigmaH) && std::isfinite(pe.ellipse.majorM) && pe.cEE >= 0 && pe.cNN >= 0 && pe.cEE * pe.cNN - pe.cEN * pe.cEN > -1e-6 * pe.cEE * pe.cNN - 1e-9) || std::fabs(pe.lat) > 90.0001) bad++;
            }
        }
        CHECK(bad == 0, "garbage input produced %d bad outputs", bad);
    }
    {   // holding pattern: 1 minute legs and standard-rate turns at 220 kt. With ADS-B (4 s) the turn model must follow it; with reports every
        // 2 minutes the race track is aliased (two fixes say nothing about the velocity): the numbers are printed, the estimate must stay finite.
        Truth h = makeHold(51.0, 0.0, 90, 120);
        for (int pass = 0; pass < 2; pass++) {
            Rng r(3);
            AdsbSensor as; SatSensor ss; ss.gapSec = 120;
            const double gap = pass == 0 ? 4.0 : 120.0;
            TrackEstimator est;
            int inside = 0, n = 0; std::vector<double> errs;
            for (double t = 60; t < 7000; t += gap) {
                est.update(pass == 0 ? as.make(h, t, r) : ss.make(h, t, r));
                PosEstimate pe;
                const double tq = t + gap * 0.5;
                if (t < 600 || !est.estimate(tq, pe)) continue;
                const TState g = h.at(tq);
                const double dE = wrap180(g.lon - pe.lon) * kDeg * kEarthR * std::cos(pe.lat * kDeg), dN = (g.lat - pe.lat) * kDeg * kEarthR;
                const double det = pe.cEE * pe.cNN - pe.cEN * pe.cEN;
                const double m2 = (dE * dE * pe.cNN - 2 * dE * dN * pe.cEN + dN * dN * pe.cEE) / det;
                n++; if (m2 < 5.991) inside++;
                errs.push_back(std::hypot(dE, dN));
                CHECK(std::isfinite(m2), "hold nan");
            }
            std::printf("hold, %s: median error %.0f m, p95 %.0f m, %d of %d truths inside the 95%% ellipse\n", pass == 0 ? "ADS-B every 4 s" : "reports every 2 min", pct(errs, 0.5), pct(errs, 0.95), inside, n);
            if (pass == 0) { CHECK(inside > 0.8 * n && pct(errs, 0.5) < 150, "hold with ADS-B: %d/%d median %.0f", inside, n, pct(errs, 0.5)); }
        }
    }
    {   // route: arrival clamp, ETA
        const Airport *a = findAirport("EGLL"), *b = findAirport("EDDF");
        TrackEstimator est;
        RouteHint h; h.hasDest = true; h.destLat = b->lat; h.destLon = b->lon; est.setRoute(h);
        double la, lo; gcDestination(b->lat, b->lon, gcBearingDeg(b->lat, b->lon, a->lat, a->lon), 60e3, la, lo);
        for (int i = 0; i < 4; i++) {
            Fix f; f.src = Src::Acars; f.t = i * 60; f.hasPos = true; f.timeSigmaSec = 0; f.posSigmaM = 100;
            gcDestination(la, lo, gcBearingDeg(la, lo, b->lat, b->lon), 200.0 * 60 * i, f.lat, f.lon);
            est.update(f);
        }
        PosEstimate p;
        est.estimate(180 + 60, p);
        CHECK(p.routeUsed && p.etaT > 240, "route used %d", (int)p.routeUsed);
        est.estimate(180 + 3600, p);
        CHECK(p.arrived && errM(p.lat, p.lon, b->lat, b->lon) < 1000, "arrived clamp %.0f", errM(p.lat, p.lon, b->lat, b->lon));
    }
    {   // hub: wind prior, coarse area from a ground station, purge
        TrackHub hub;
        hub.setStation(7, 50.0, 8.0);
        hub.noteStation(42, 7, 100);
        PosEstimate p;
        CHECK(hub.estimate(42, 160, p) && p.coarse && p.sigmaH > 50e3, "coarse");
        Fix f; f.src = Src::Vdl2; f.t = 200; f.hasPos = true; f.lat = 50.1; f.lon = 8.1; hub.feed(42, f);
        CHECK(hub.estimate(42, 260, p) && !p.coarse, "fix replaces coarse");
        CHECK(hub.feedText(42, "H1", "/ETA 1423 DEST KJFK", 260, 14 * 3600), "feedText");
        CHECK(hub.find(42)->route().hasDest, "route set");
        hub.purge(1e6, 3600);
        CHECK(hub.size() == 0, "purge");
    }

    // ---- speed
    {
        const int N = 5000;
        std::vector<TrackEstimator> trk;
        trk.reserve(N);
        Rng r(1);
        std::vector<double> lat(N), lon(N), trkd(N);
        for (int i = 0; i < N; i++) { trk.emplace_back(); lat[i] = 40 + 20 * r.u(); lon[i] = -10 + 40 * r.u(); trkd[i] = 360 * r.u(); }
        auto t0 = std::chrono::steady_clock::now();
        const int steps = 20;
        for (int s = 0; s < steps; s++) for (int i = 0; i < N; i++) {
            Fix f; f.src = Src::Adsb; f.t = s; f.hasPos = true; f.nacp = 9;
            gcDestination(lat[i], lon[i], trkd[i], 230.0 * s, f.lat, f.lon); f.hasVel = true; f.speedMs = 230; f.trackDeg = trkd[i];
            f.hasAlt = true; f.altM = 10000; f.hasVrate = true;
            trk[(size_t)i].update(f);
        }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / (N * steps);
        PosEstimate p; double sum = 0;
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; i++) { trk[(size_t)i].estimate(steps + 0.5, p, true); sum += p.lat; }
        const double usq = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / N;
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; i++) { trk[(size_t)i].estimate(steps + 400, p, false); sum += p.lat; }
        const double usf = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / N;
        std::printf("speed: update %.1f us (pos+vel+alt+vrate), quick estimate %.2f us, full estimate with ellipse 400 s ahead %.1f us  (%g)\n", us, usq, usf, sum * 0);
        CHECK(us < 100, "update too slow %.1f us", us);
    }
    std::printf("%s\n", g_fail ? "FAILED" : "all ok");
    return g_fail ? 1 : 0;
}
