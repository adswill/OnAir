// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Interference hunter: spur and comb finding, classification (switching supply, LED lamp, powerline, clock / oscillator leak), the monitor
// (baseline, new-interference alerts, floor rise, clearing, no false alarms), the walk meter and logs, and the NMEA parser. Synthetic sweeps only.
#include "../common/tmp_path.h"
#include "dect2/rfi/rfi.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace dect2::rfi;
using dect2::sweep::Sweep;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static SimSource comb(double f0, double sp, int teeth, float lvl, double on = 0, double off = 1e18) {
    SimSource s; s.type = SimSource::Comb; s.freqHz = f0; s.spacingHz = sp; s.teeth = teeth; s.levelDb = lvl; s.onSec = on; s.offSec = off; return s;
}
static SimSource hump(double f0, double w, float lvl, double on = 0, double off = 1e18, bool plc = false) {
    SimSource s; s.type = plc ? SimSource::PlcHf : SimSource::Hump; s.freqHz = f0; s.widthHz = w; s.levelDb = lvl; s.onSec = on; s.offSec = off; return s;
}
static SimSource tone(double f, float lvl, double on = 0, double off = 1e18, bool lo = false) {
    SimSource s; s.type = lo ? SimSource::Lo : SimSource::Carrier; s.freqHz = f; s.levelDb = lvl; s.onSec = on; s.offSec = off; return s;
}

static const Finding* find(const std::vector<Finding>& v, Kind k) { for (const Finding& f : v) if (f.kind == k) return &f; return nullptr; }

static std::vector<Finding> classifyScene(const SimScene& sc, double t = 10) {
    // a baseline of the quiet scene (the same scene with only the carriers), then the sweep with everything on
    SimScene quiet = sc;
    quiet.sources.clear();
    for (const SimSource& s : sc.sources) if (s.type == SimSource::Carrier) quiet.sources.push_back(s);
    std::vector<float> ref = simulateSweep(quiet, 0).db;
    const Sweep s = simulateSweep(sc, t);
    SpurConfig spc;
    return classify(findSpurs(s, spc), findHumps(s, ref, 4, 400e3), s.binHz);
}

static void testClassify() {
    for (double bin : {10e3, 25e3, 50e3}) {
        for (double off : {0.0, 0.37, 0.71}) {   // teeth not on bin centres
            const double sp = std::max(100e3, 5 * bin);   // a comb needs a spacing of at least 5 bins to be told from a wide signal
            SimScene sc; sc.binHz = bin; sc.seed = 3;
            sc.sources.push_back(tone(98.0e6, 40));   // a broadcast carrier
            sc.sources.push_back(comb(91.2e6 + off * bin, sp, 9, 24));
            const auto f = classifyScene(sc);
            const Finding* s = find(f, Kind::SwitchingSupply);
            CHECK(s != nullptr, "switching supply not found (bin %.0f off %.2f)", bin, off);
            if (s) {
                CHECK(std::fabs(s->spacingHz - sp) < 0.01 * sp + 0.2 * bin, "spacing %.0f (bin %.0f)", s->spacingHz, bin);
                CHECK(s->count >= 7, "teeth %d", s->count);
            }
            CHECK(!find(f, Kind::LedLamp), "a comb alone is not an LED lamp");
        }
    }
    {   // LED lamp: broadband hump with a comb inside
        SimScene sc; sc.seed = 4;
        sc.sources.push_back(hump(90e6, 5e6, 9));
        sc.sources.push_back(comb(90.5e6, 150e3, 12, 22));
        const auto f = classifyScene(sc);
        const Finding* l = find(f, Kind::LedLamp);
        CHECK(l != nullptr && l->confidence >= 0.7f, "LED lamp %s conf %.2f", l ? "found" : "missing", l ? l->confidence : 0.f);
        CHECK(!find(f, Kind::SwitchingSupply), "the comb inside the hump belongs to the lamp");
        CHECK(l && l->spacingHz > 145e3 && l->spacingHz < 155e3, "lamp spacing");
    }
    {   // powerline adapter: HF block with notches at amateur bands
        SimScene sc; sc.startHz = 1.0e6; sc.stopHz = 30e6; sc.binHz = 50e3; sc.seed = 5;
        sc.sources.push_back(hump(2.0e6, 26e6, 12, 0, 1e18, true));
        const auto f = classifyScene(sc);
        const Finding* p = find(f, Kind::Powerline);
        CHECK(p != nullptr && p->hamNotches >= 2, "powerline %s notches %d", p ? "found" : "missing", p ? p->hamNotches : -1);
        CHECK(!find(f, Kind::LedLamp), "a notched HF block is not a lamp");
    }
    {   // the same block without notches is not called powerline
        SimScene sc; sc.startHz = 1.0e6; sc.stopHz = 30e6; sc.binHz = 50e3; sc.seed = 6;
        sc.sources.push_back(hump(2.0e6, 26e6, 12));
        const auto f = classifyScene(sc);
        CHECK(!find(f, Kind::Powerline), "no notches: not powerline");
    }
    {   // clock leak: on a harmonic of 25 MHz, and an off-grid spur
        SimScene sc; sc.seed = 7; sc.startHz = 90e6; sc.stopHz = 110e6;
        sc.sources.push_back(tone(100.0e6, 30, 0, 1e18, true));
        const auto f = classifyScene(sc);
        const Finding* l = find(f, Kind::LoLeak);
        CHECK(l != nullptr && l->clockHz > 0, "LO leak on a clock harmonic");
        SimScene sd = sc; sd.sources.clear();
        sd.sources.push_back(tone(97.3125e6, 30, 0, 1e18, true));
        const auto g = classifyScene(sd);
        const Finding* m = find(g, Kind::LoLeak);
        CHECK(m != nullptr && m->clockHz == 0 && (!l || m->confidence < l->confidence), "LO leak off the clock grid is less sure");
    }
    {   // random unrelated spurs are not a comb
        SimScene sc; sc.seed = 8;
        for (double f : {88.3e6, 89.9e6, 93.1e6, 95.77e6, 99.4e6, 104.2e6, 106.55e6}) sc.sources.push_back(tone(f, 25));
        SimScene noCarr = sc;
        const Sweep s = simulateSweep(noCarr, 5);
        const auto sp = findSpurs(s);
        CHECK(sp.size() == 7, "seven spurs found: %zu", sp.size());
        const Comb c = findComb(sp, s.binHz);
        CHECK(!c.ok(), "unrelated spurs do not make a comb (hits %d spacing %.0f)", c.hits, c.spacingHz);
    }
}

static void testMonitor() {
    SimScene sc; sc.seed = 11;
    sc.sources.push_back(tone(88.5e6, 45)); sc.sources.push_back(tone(95.2e6, 38)); sc.sources.push_back(tone(101.7e6, 42));
    sc.sources.push_back(comb(91.2e6, 100e3, 9, 24, 300, 400));      // switching supply from sweep 30 to 40
    sc.sources.push_back(hump(96e6, 6e6, 10, 500, 600));              // LED lamp from sweep 50 to 60
    sc.sources.push_back(tone(104.1e6, 30, 700, 1e18, true));        // oscillator leak from sweep 70 on
    Monitor m;
    std::vector<Alert> all;
    for (int i = 0; i < 90; i++) {
        const double t = i * 10.0;
        const auto a = m.add(simulateSweep(sc, t));
        for (const Alert& x : a) all.push_back(x);
        if (i < 5) CHECK(m.learning() == (i < 4), "learning state at %d", i);
        if (i < 29) CHECK(a.empty(), "false alarm at sweep %d", i);
    }
    int newSp = 0, humps = 0, cleared = 0, comb = 0, lo = 0;
    for (const Alert& a : all) {
        if (a.type == Alert::NewSpur) { newSp++; if (a.kind == Kind::SwitchingSupply) comb++; if (a.kind == Kind::LoLeak) lo++; }
        if (a.type == Alert::NewHump) { humps++; CHECK(a.kind == Kind::LedLamp, "hump kind %d", (int)a.kind); }
        if (a.type == Alert::Cleared) cleared++;
    }
    CHECK(comb >= 7, "comb teeth alerted as a switching supply: %d", comb);
    CHECK(lo == 1, "one oscillator leak alert: %d", lo);
    CHECK(humps == 1, "one broadband alert: %d", humps);
    CHECK(cleared >= 3, "alerts cleared: %d", cleared);
    CHECK(newSp == comb + lo, "no other spur alerts (%d vs %d)", newSp, comb + lo);
    // time to alert: the comb starts at sweep 30, confirmSweeps 2 -> alerted at sweep 31
    double firstComb = -1;
    for (const Alert& a : all) if (a.type == Alert::NewSpur && a.kind == Kind::SwitchingSupply) { firstComb = a.time; break; }
    CHECK(std::fabs(firstComb - 310) < 1, "comb alert time %.0f", firstComb);
    CHECK(!m.history().empty() && m.log().size() == all.size(), "history and log kept");
    // accept: the lamp becomes normal
    Monitor m2;
    SimScene s2 = sc; s2.sources.clear(); s2.sources.push_back(tone(88.5e6, 45)); s2.sources.push_back(tone(104.1e6, 30, 100, 1e18, true));
    for (int i = 0; i < 20; i++) m2.add(simulateSweep(s2, i * 10.0));
    CHECK(m2.log().size() == 1, "leak alerted once");
    m2.accept();
    size_t before = m2.log().size();
    for (int i = 20; i < 40; i++) m2.add(simulateSweep(s2, i * 10.0));
    CHECK(m2.log().size() == before && m2.active().empty(), "accepted leak raises nothing more");
    // a quiet band over many sweeps: no false alarms at any bin width
    for (double bin : {10e3, 25e3, 100e3}) {
        SimScene q; q.binHz = bin; q.seed = 21; q.sources.push_back(tone(98e6, 40));
        Monitor mq;
        size_t alerts = 0;
        for (int i = 0; i < 150; i++) alerts += mq.add(simulateSweep(q, i * 5.0)).size();
        CHECK(alerts == 0, "false alarms on a quiet band (bin %.0f): %zu", bin, alerts);
    }
    // a different range restarts the baseline
    SimScene r = sc; r.startHz = 430e6; r.stopHz = 440e6;
    m.add(simulateSweep(r, 2000));
    CHECK(m.learning(), "new range: learning again");
    // floor rise: a whole-band lift of 5 dB
    {
        SimScene fl; fl.seed = 31; fl.sources.push_back(hump(87e6, 21e6, 5, 400, 1e18));
        fl.sources.push_back(tone(98e6, 40));
        Monitor mf;
        bool rise = false;
        for (int i = 0; i < 80; i++) for (const Alert& a : mf.add(simulateSweep(fl, i * 10.0))) if (a.type == Alert::FloorRise || a.type == Alert::NewHump) rise = true;
        CHECK(rise, "a band-wide lift is reported");
    }
    // exports
    std::string err;
    CHECK(m.exportLogCsv(tmpPath("rfi_test_log.csv"), err), "log csv %s", err.c_str());
    CHECK(m.exportFloorCsv(tmpPath("rfi_test_floor.csv"), err), "floor csv");
    std::remove(tmpPath("rfi_test_log.csv")); std::remove(tmpPath("rfi_test_floor.csv"));
    // degenerate input
    Sweep e;
    CHECK(m.add(e).empty(), "empty sweep ignored");
}

static void testRobust() {
    // many noise seeds and bin widths: the comb is found every time and a quiet band never raises an alert
    int missed = 0, wrongKind = 0, falseAlerts = 0;
    for (uint64_t seed = 1; seed <= 20; seed++)
        for (double bin : {10e3, 25e3, 50e3}) {
            SimScene sc; sc.binHz = bin; sc.seed = seed; sc.floorDb = -70 - (float)(seed % 7);
            sc.sources.push_back(tone(88.5e6, 45)); sc.sources.push_back(tone(101.7e6, 42));
            sc.sources.push_back(comb(93.05e6 + 3e3 * (double)(seed % 5), std::max(120e3, 5 * bin), 8, 20, 100, 1e18));
            Monitor m;
            int comb = 0, wrong = 0;
            for (int i = 0; i < 25; i++)
                for (const Alert& a : m.add(simulateSweep(sc, i * 10.0))) {
                    if (i * 10.0 < 100) falseAlerts++;
                    else if (a.type == Alert::NewSpur) { comb++; if (a.kind != Kind::SwitchingSupply) wrong++; }
                }
            if (comb < 6) missed++;
            if (wrong) wrongKind++;
        }
    CHECK(missed == 0, "combs missed: %d of 60", missed);
    CHECK(wrongKind == 0, "combs named wrongly: %d of 60", wrongKind);
    CHECK(falseAlerts == 0, "false alarms before the interference: %d", falseAlerts);
    // LED lamps and powerline adapters across noise seeds, floor levels and bin widths
    int ledMiss = 0, plcMiss = 0, plcWrong = 0;
    for (uint64_t seed = 1; seed <= 20; seed++)
        for (double bin : {10e3, 25e3, 50e3}) {
            SimScene l; l.binHz = bin; l.seed = seed; l.floorDb = -75 + (float)(seed % 9);
            l.sources.push_back(tone(98e6, 40));
            l.sources.push_back(hump(92e6 + 50e3 * (double)(seed % 4), 3e6 + 1e5 * (double)seed, 7 + (float)(seed % 5)));
            l.sources.push_back(comb(92.3e6, std::max(100e3, 5 * bin), 10, 20));
            if (!find(classifyScene(l), Kind::LedLamp)) ledMiss++;
            SimScene p; p.startHz = 1e6; p.stopHz = 30e6; p.binHz = std::max(bin, 25e3); p.seed = seed; p.floorDb = l.floorDb;
            p.sources.push_back(hump(2.0e6, 26e6, 8 + (float)(seed % 6), 0, 1e18, true));
            const auto fp = classifyScene(p);
            if (!find(fp, Kind::Powerline)) plcMiss++;
            if (find(fp, Kind::LedLamp)) plcWrong++;
        }
    CHECK(ledMiss == 0, "LED lamps missed: %d of 60", ledMiss);
    CHECK(plcMiss == 0 && plcWrong == 0, "powerline missed %d, called a lamp %d (of 60)", plcMiss, plcWrong);
    // gain drift: the floor moves 2 dB (no alert), then 6 dB (alert)
    SimScene a; a.seed = 5; a.sources.push_back(tone(98e6, 40));
    Monitor m;
    for (int i = 0; i < 10; i++) m.add(simulateSweep(a, i));
    SimScene b = a; b.floorDb += 2;
    size_t n = 0;
    for (int i = 10; i < 20; i++) n += m.add(simulateSweep(b, i)).size();
    CHECK(n == 0, "a 2 dB drift is not an alert (%zu)", n);
    SimScene c = a; c.floorDb += 7;
    bool rise = false;
    for (int i = 20; i < 30; i++) for (const Alert& x : m.add(simulateSweep(c, i))) if (x.type == Alert::FloorRise) rise = true;
    CHECK(rise, "a 7 dB rise of the floor is an alert");
}

static void testWalk() {
    SimScene quiet; quiet.seed = 41; quiet.sources.push_back(tone(98e6, 40));
    Walk w;
    for (int i = 0; i < 8; i++) { const double la = 54.0, lo = 25.0 + 1e-4 * i; w.add(simulateSweep(quiet, i), &la, &lo); }
    CHECK(w.meter() < 0.1f, "quiet meter %.2f", w.meter());
    w.ignoreNow();   // the broadcast carrier is normal
    for (int i = 0; i < 6; i++) { const double la = 54.0, lo = 25.0; w.add(simulateSweep(quiet, 20 + i), &la, &lo); }
    // walking towards a source: the interference grows 25 dB over 20 readings
    float last = 0;
    for (int i = 0; i < 20; i++) {
        SimScene sc = quiet;
        sc.sources.push_back(hump(90e6, 6e6, (float)(1.25 * (i + 1))));
        sc.sources.push_back(comb(91e6, 80e3, 9, (float)(1.25 * (i + 1) + 10)));
        const double la = 54.0, lo = 25.001 + 1e-4 * i;
        w.add(simulateSweep(sc, 30 + i), &la, &lo);
        if (i == 3) CHECK(w.trend() >= 0, "trend");
        last = w.meter();
    }
    CHECK(last > 0.5f, "loud meter near the source: %.2f", last);
    CHECK(w.trend() == 1, "getting louder");
    CHECK(w.loudest() >= 30, "loudest reading is near the end: %d", w.loudest());
    CHECK(w.readings().back().kind == Kind::SwitchingSupply || w.readings().back().kind == Kind::LedLamp, "a source is named in the log: %d", (int)w.readings().back().kind);
    CHECK(w.readings().back().peakDb > 10, "strongest spur recorded");
    // walking away: quieter
    for (int i = 0; i < 12; i++) { const double la = 54.0, lo = 25.1; w.add(simulateSweep(quiet, 100 + i), &la, &lo); }
    CHECK(w.trend() <= 0 && w.meter() < 0.3f, "quieter after leaving: %.2f", w.meter());
    // manual reference and ignoring a known carrier
    w.setReferenceNow();
    CHECK(w.referenceDb() == w.smoothedDb(), "reference set");
    const size_t n = w.readings().size();
    w.ignoreNow();
    w.add(simulateSweep(quiet, 200), nullptr, nullptr);
    CHECK(w.readings().size() == n + 1 && !w.readings().back().hasPos, "reading without a position");
    std::string err;
    CHECK(w.exportCsv(tmpPath("rfi_walk.csv"), err), "walk csv");
    CHECK(w.exportGpx(tmpPath("rfi_walk.gpx"), err), "walk gpx");
    std::ifstream f(tmpPath("rfi_walk.csv")); std::stringstream ss; ss << f.rdbuf();
    int lines = 0; for (char c : ss.str()) if (c == '\n') lines++;
    CHECK((size_t)lines == n + 2, "csv lines %d vs %zu", lines, n + 2);
    std::ifstream g(tmpPath("rfi_walk.gpx")); std::stringstream gs; gs << g.rdbuf();
    size_t wpt = 0, pos = 0; while ((pos = gs.str().find("<wpt ", pos)) != std::string::npos) { wpt++; pos++; }
    CHECK(wpt == n, "gpx waypoints %zu vs %zu", wpt, n);
    std::remove(tmpPath("rfi_walk.csv")); std::remove(tmpPath("rfi_walk.gpx"));
    w.clear();
    CHECK(w.readings().empty() && w.loudest() == -1, "cleared");
}

static void testNmea() {
    Fix f;
    CHECK(parseNmea("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47", f) && f.valid, "GGA");
    CHECK(std::fabs(f.lat - 48.1173) < 1e-4 && std::fabs(f.lon - 11.51667) < 1e-4 && f.sats == 8, "GGA position %.5f %.5f", f.lat, f.lon);
    CHECK(std::fabs(f.utcSec - (12 * 3600 + 35 * 60 + 19)) < 1e-6, "GGA time");
    Fix g;
    CHECK(parseNmea("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n", g) && g.valid && std::fabs(g.lat - 48.1173) < 1e-4, "RMC");
    Fix h;
    CHECK(!parseNmea("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48", h), "bad checksum refused");
    CHECK(parseNmea("$GNRMC,123519,V,,,,,,,230394,,*00", h) == false || !h.valid, "no fix");
    Fix s;
    CHECK(parseNmea("$GNGGA,010203.00,5400.000,S,02500.000,W,2,10,0.8,10,M,0,M,,", s) && s.lat < -53.9 && s.lon < -24.9, "south west, no checksum");
    CHECK(!parseNmea("$GPGSV,3,1,11,03,03,111,00*74", h), "other sentences ignored");
    CHECK(!parseNmea("garbage", h) && !parseNmea("", h) && !parseNmea("$", h), "garbage");
}

int main() {
    testClassify();
    testMonitor();
    testRobust();
    testWalk();
    testNmea();
    printf(fails ? "FAILED %d\n" : "rfi: all passed\n", fails);
    return fails ? 1 : 0;
}
