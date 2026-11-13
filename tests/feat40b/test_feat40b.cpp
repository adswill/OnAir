// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 4.0 Lane B group 2: talkgroup activity / follow, radiosonde launch clock, ADS-B track replay, transmit planning. Simulated data only.
#include "dect2/adsb/adsb_replay.h"
#include "dect2/sonde/sonde_launch_clock.h"
#include "dect2/trunkpool/tg_activity.h"
#include "dect2/packet/packet_beacons.h"
#include "dect2/tx/tx_plan.h"
#include <cstdio>
#include <random>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
using namespace dect2;
static bool near_(double a, double b, double e) { return std::fabs(a - b) <= e; }

static void tgTest() {
    TgActivity a;
    a.add(100, 1, 0, 10); a.add(100, 2, 20, 25); a.add(200, 3, 5, 9); a.add(0, 1, 0, 5); a.add(300, 1, 10, 5);   // tg 0 and reversed ignored
    CHECK(a.size() == 3);
    auto r = a.ranking(30, 60);
    CHECK(r.size() == 2 && r[0].tg == 100 && near_(r[0].airtimeS, 15, 1e-9) && r[0].calls == 2 && r[0].radios == 2);
    r = a.ranking(30, 8);   // window [22,30]: only 3 s of the call 20..25
    CHECK(r.size() == 1 && near_(r[0].airtimeS, 3, 1e-9));
    auto tl = a.timeline(100, 30, 10, 3);   // buckets 0-10, 10-20, 20-30
    CHECK(tl.size() == 3 && near_(tl[0], 10, 1e-9) && near_(tl[1], 0, 1e-9) && near_(tl[2], 5, 1e-9));
    CHECK(a.timeline(100, 30, 0, 3).size() == 3 && a.timeline(100, 30, 10, 0).empty());
    // follower: hysteresis, minimum dwell, nothing busy keeps the current one
    TgActivity b; TgFollower f; f.windowS = 100;
    CHECK(f.pick(b, 0) == 0);
    b.add(1, 1, 0, 20);
    CHECK(f.pick(b, 20) == 1);
    b.add(2, 2, 21, 40);   // tg 2 now has 19 s, tg 1 has 20 s: no switch
    CHECK(f.pick(b, 40) == 1);
    b.add(2, 2, 41, 80);   // 58 s vs 20 s, dwell (40 - 20) < 30 at t=45: stays; at t=80 switches
    CHECK(f.pick(b, 45) == 1);
    CHECK(f.pick(b, 80) == 2);
    CHECK(f.pick(b, 200) == 2);   // everything is older than the window: keep following
    // alternating talkgroups do not make the follower flap: count switches over a simulated hour
    TgActivity c; TgFollower g; g.windowS = 300; int sw = 0; uint32_t last = 0; std::mt19937 rng(7);
    for (double t = 0; t < 3600; t += 10) {
        c.add((rng() & 1) ? 10 : 20, 1, t, t + 4 + (rng() % 3));
        const uint32_t p = g.pick(c, t + 8);
        if (p != last) { sw++; last = p; }
    }
    CHECK(sw <= 12);
    printf("follower switches in an hour of two equal talkgroups: %d\n", sw);
}

static void sondeTest() {
    const int64_t day = 86400;
    std::vector<int64_t> l;
    std::mt19937 rng(3);
    for (int d = 10; d < 16; d++) { l.push_back(d * day + 11 * 3600 + 15 * 60 + (int)(rng() % 600) - 300); l.push_back(d * day + 23 * 3600 + 20 * 60 + (int)(rng() % 600) - 300); }
    const int64_t now = 16 * day + 9 * 3600;   // 09:00 on day 16
    auto p = predictNextLaunch(l, now);
    CHECK(p.valid && p.samples == 6);
    CHECK(near_((double)(p.nextUtc - 16 * day) / 60.0, 11 * 60 + 15, 6));   // ~11:15 that day
    CHECK(p.listenFromUtc < p.nextUtc - 14 * 60);
    CHECK(!launchWindowOpen(p, now) && launchWindowOpen(p, p.nextUtc - 10 * 60) && !launchWindowOpen(p, p.nextUtc + 3 * 3600));
    p = predictNextLaunch(l, 16 * day + 12 * 3600);   // after the first slot: the evening one
    CHECK(near_((double)(p.nextUtc - 16 * day) / 60.0, 23 * 60 + 20, 6));
    p = predictNextLaunch(l, 16 * day + 23 * 3600 + 59 * 60);   // after both: tomorrow's 11:15
    CHECK(near_((double)(p.nextUtc - 17 * day) / 60.0, 11 * 60 + 15, 6));
    CHECK(!predictNextLaunch({}, now).valid);
    std::vector<int64_t> few = {10 * day + 3600, 11 * day + 3600};   // two days: not regular yet
    CHECK(!predictNextLaunch(few, now).valid);
    // a slot that straddles midnight (23:50 and 00:10) is one slot
    std::vector<int64_t> m;
    for (int d = 10; d < 15; d++) m.push_back(d * day + (d % 2 ? 23 * 3600 + 50 * 60 : 24 * 3600 + 10 * 60));
    p = predictNextLaunch(m, 15 * day + 3600);
    CHECK(p.valid && p.samples == 5);
    // launches scattered at random times of day make no slot
    std::vector<int64_t> r;
    for (int d = 0; d < 6; d++) r.push_back(d * day + (int)(rng() % 86400));
    int valid = predictNextLaunch(r, 7 * day).valid ? 1 : 0;
    printf("random launch times give a prediction: %d (0 expected, 1 would be a coincidence)\n", valid);
}

static void replayTest() {
    TrackReplay t;
    CHECK(t.add(1, 100, 50.0, 10.0, 30000));
    CHECK(t.add(1, 200, 50.1, 10.2, 31000));         // 17 km in 100 s: 620 km/h
    CHECK(!t.add(1, 200, 50.1, 10.2, 31000));       // not newer
    CHECK(!t.add(1, 220, 0, 0, 0));                  // null island
    CHECK(!t.add(1, 201, 55.0, 10.2, 31000));        // 550 km in 1 s: bad decode
    CHECK(!t.add(1, 202, 95.0, 10.2, 0));
    auto p = t.at(1, 150);
    CHECK(p.valid && p.interpolated && near_(p.lat, 50.05, 1e-9) && near_(p.lon, 10.1, 1e-9) && near_(p.altFt, 30500, 1e-6));
    CHECK(t.at(1, 100).valid && !t.at(1, 100).interpolated);
    CHECK(!t.at(1, 99).valid && !t.at(1, 201).valid && !t.at(2, 150).valid);
    t.add(1, 600, 50.2, 10.4, 33000);   // 400 s hole: not bridged
    CHECK(!t.at(1, 400).valid && t.at(1, 600).valid);
    double a, b;
    CHECK(t.span(1, a, b) && a == 100 && b == 600 && !t.span(9, a, b));
    t.add(2, 150, 40.0, 5.0, 0);
    CHECK(t.sky(150).size() == 2 && t.sky(50).empty());
    t.prune(300);
    CHECK(t.aircraft() == 1);
    TrackReplay w;   // antimeridian
    w.add(5, 0, 10, 179.95, 0); w.add(5, 100, 10, -179.95, 0);
    auto q = w.at(5, 50);
    CHECK(q.valid && std::fabs(std::fabs(q.lon) - 180) < 0.01);
    TrackReplay cap; cap.maxFixes = 50;
    for (int i = 1; i <= 200; i++) cap.add(7, i, 10 + i * 0.0001, 10, 0);
    CHECK(cap.fixes(7) == 50 && !cap.at(7, 100).valid && cap.at(7, 190).valid);
}

static void txTest() {
    CHECK(near_(fsplDb(1, 100), 72.44, 0.01) && near_(fsplDb(10, 100), 92.44, 0.01) && fsplDb(0, 100) == 0);
    LinkBudget b; b.txDbm = 30; b.txLossDb = 1; b.txGainDbi = 3; b.rxGainDbi = 2; b.rxLossDb = 0.5; b.rxSensDbm = -110;
    auto r = linkBudget(b, 10, 145);
    CHECK(near_(r.eirpDbm, 32, 1e-9) && near_(r.pathLossDb, fsplDb(10, 145), 1e-9));
    CHECK(near_(r.marginDb, r.rxDbm + 110, 1e-9));
    auto edge = linkBudget(b, r.maxKm, 145);   // at maxKm the margin is zero
    CHECK(near_(edge.marginDb, 0, 1e-6));
    b.extraLossDb = 20; CHECK(linkBudget(b, 10, 145).maxKm < r.maxKm / 5);
    auto w = planCheck(61.0);   // 2nd harmonic 122 MHz airband, 6th is out of range
    CHECK(w.size() == 1 && near_(w[0].mhz, 122.0, 1e-9));
    w = planCheck(121.5); CHECK(w.size() >= 2);      // carrier is both in airband and the emergency channel
    w = planCheck(145.0); CHECK(w.empty());          // 2m: harmonics 290, 435, 580, 725: clean
    w = planCheck(203.0); bool ep = false; for (auto& x : w) if (x.what.find("EPIRB") != std::string::npos) ep = true;
    CHECK(ep);                                        // 2 x 203 = 406
    CHECK(planCheck(0).empty() && planCheck(-5).empty());
    auto rp = powerRamp(0, 20, 5);
    CHECK(rp.size() == 5 && rp.front() == 0 && rp.back() == 20);
    rp = powerRamp(0, 18, 5); CHECK(rp.size() == 5 && rp.back() == 18);
    CHECK(powerRamp(30, 10, 5).size() == 1 && powerRamp(0, 10, 0).size() == 1);
    CHECK(near_(reflectedFraction(1.0), 0, 1e-12) && near_(reflectedFraction(3.0), 0.25, 1e-12) && reflectedFraction(0.2) == 0);
    CHECK(swrBackoffDb(1.2) == 0 && near_(swrBackoffDb(3.0), 3.0, 1e-9) && swrBackoffDb(9) == 10);
    double prev = 0; bool mono = true;
    for (double s = 1; s < 8; s += 0.1) { const double x = swrBackoffDb(s); if (x + 1e-9 < prev) mono = false; prev = x; }
    CHECK(mono);
}

static PacketFrameInfo fr(const char* from, double t, const char* info, const char* path = "", int baud = 1200) {
    PacketFrameInfo f; f.from = from; f.timeSec = t; f.info = info; f.path = path; f.baud = baud; return f;
}
static void beaconTest() {
    std::vector<PacketFrameInfo> v;
    for (int i = 0; i < 20; i++) v.push_back(fr("FAST-9", 100 + i * 20.0, ("!5430.00N/02518.00E>" + std::to_string(i)).c_str(), "WIDE1-1,WIDE2-1"));   // every 20 s, changing text
    for (int i = 0; i < 6; i++) v.push_back(fr("SPAM-1", 100 + i * 30.0, "!5430.00N/02518.00E>stuck"));                                           // same text every 30 s
    for (int i = 0; i < 3; i++) v.push_back(fr("QUIET-1", 100 + i * 600.0, "!5400.00N/02500.00E-home"));                                         // normal fixed station
    v.push_back(fr("OLD-1", 1.0, "x"));                                                                                                             // outside the window
    v.push_back(fr("", 150, "no source"));
    std::reverse(v.begin(), v.end());   // any order
    auto u = analyseChannel(v, 1300, 1200);
    CHECK(u.stations.size() == 3);
    const BeaconStat* fast = nullptr; const BeaconStat* spam = nullptr; const BeaconStat* quiet = nullptr;
    for (const auto& s : u.stations) { if (s.call == "FAST-9") fast = &s; if (s.call == "SPAM-1") spam = &s; if (s.call == "QUIET-1") quiet = &s; }
    CHECK(fast && spam && quiet);
    if (fast && spam && quiet) {
        CHECK(fast->frames == 20 && near_(fast->medianGapS, 20, 1e-9) && fast->fast && !fast->repeat);
        CHECK(spam->repeat && spam->repeats == 5 && spam->fast);   // 30 s gaps are also "fast"
        CHECK(!quiet->fast && !quiet->repeat && !quiet->hog && near_(quiet->medianGapS, 600, 1e-9) && near_(quiet->minGapS, 600, 1e-9));
        CHECK(u.stations.front().call == "FAST-9");   // most airtime first
    }
    CHECK(u.occupancyPct > 0 && u.occupancyPct < 50);
    // a hog: a 200-byte frame every 3 s at 1200 baud uses most of the channel
    std::vector<PacketFrameInfo> h;
    for (int i = 0; i < 100; i++) h.push_back(fr("HOG-1", i * 3.0, std::string(200, 'x').c_str()));
    auto uh = analyseChannel(h, 300, 300);
    CHECK(uh.stations.size() == 1 && uh.stations[0].hog && uh.occupancyPct > 60);
    // 9600 baud is about eight times shorter
    CHECK(frameAirtimeS(fr("A", 0, "0123456789", "", 9600)) < frameAirtimeS(fr("A", 0, "0123456789", "", 1200)) / 4);
    CHECK(analyseChannel({}, 100, 100).stations.empty() && analyseChannel(v, 100, 0).stations.empty());
    // single frame: no gap statistics, no flags
    auto u1 = analyseChannel({fr("ONE-1", 5, "t")}, 10, 10);
    CHECK(u1.stations.size() == 1 && u1.stations[0].medianGapS == 0 && !u1.stations[0].fast);
}

int main() {
    beaconTest(); tgTest(); sondeTest(); replayTest(); txTest();
    printf(fails ? "FAILED\n" : "all ok\n");
    return fails ? 1 : 0;
}
