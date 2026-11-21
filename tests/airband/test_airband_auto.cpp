// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Autonomous airband scanning (4.5): no channel list. The decision logic (which carriers open, which go, guard, continuous, splatter),
// the learned list in text, the hopper (tiles, stays on a transmission, comes back to busy captures), and the receiver end to end with
// synthetic multi-carrier AM on the 25 kHz and 8.33 kHz raster with a tuning error: auto opens what keys up, names it right, the guard is
// always open, a listen request opens a carrier without a list, and with auto off nothing changes.
#include "dect2/airband/airband_auto.h"
#include "dect2/airband/airband_gen.h"
#include "dect2/airband/airband_rx.h"
#include "dect2/airband/airband_store.h"
#include "dect2/airband/airband_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static AirbandSeen seenAt(double f, bool n833, float excess, uint32_t hits, bool now = true) {
    AirbandSeen s; s.freqHz = f; s.is833 = n833; s.name = airbandName(f, n833); s.excessDb = excess; s.hits = hits; s.activeNow = now; s.lastSec = 0;
    return s;
}

static void logic() {
    AirbandAuto a;
    const double lo = 117.9e6, hi = 120.0e6;   // below the guard channels, so only the candidates can be added
    // a carrier present twice opens; once it does not (except guard), nor does one that is not on now
    auto st = a.step(10, {seenAt(118.5e6, false, 20, 2), seenAt(119.1e6, false, 20, 1), seenAt(119.2e6, false, 20, 5, false)}, {}, lo, hi);
    CHECK(st.add.size() == 1 && st.add[0].freqHz == 118.5e6 && st.add[0].label == "auto", "one opens: %zu", st.add.size());
    // on the 8.33 raster too
    st = a.step(10, {seenAt(118.0083333e6, true, 20, 2)}, {}, lo, hi);
    CHECK(st.add.size() == 1 && st.add[0].is833, "8.33 opens");
    // already open: not added again
    AirbandAutoView v; v.freqHz = 118.5e6; v.madeSec = 9; v.active = true; v.peakExcessDb = 30;
    st = a.step(10, {seenAt(118.5e6, false, 20, 4)}, {v}, lo, hi);
    CHECK(st.add.empty(), "no duplicate");
    // splatter: 8.33 kHz from a stronger active auto channel (12 dB or more)
    st = a.step(10, {seenAt(118.5083333e6, true, 12, 3)}, {v}, lo, hi);
    CHECK(st.add.empty(), "splatter of a strong neighbour is not opened");
    st = a.step(10, {seenAt(118.5083333e6, true, 25, 3)}, {v}, lo, hi);
    CHECK(st.add.size() == 1, "a neighbour of similar strength is a station");
    // two seen at once, one 15 dB weaker next to the other: only the strong one
    st = a.step(10, {seenAt(119.0e6, false, 35, 3), seenAt(119.0083333e6, true, 15, 3)}, {}, lo, hi);
    CHECK(st.add.size() == 1 && st.add[0].freqHz == 119.0e6, "heterodyne/splatter pair: %zu", st.add.size());
    // the guard is always added when inside the capture, with priority
    st = a.step(10, {}, {}, 120e6, 122e6);
    CHECK(st.add.size() == 1 && st.add[0].freqHz == 121.5e6 && st.add[0].priority, "guard 121.5");
    st = a.step(10, {}, {}, 230e6, 250e6);
    CHECK(st.add.size() == 1 && st.add[0].freqHz == 243.0e6, "guard 243.0");
    // idle auto channels go after the keep time, active ones and the guard stay
    AirbandAutoView idle; idle.freqHz = 125.0e6; idle.madeSec = 0; idle.lastEndSec = 5;
    AirbandAutoView act = idle; act.freqHz = 126.0e6; act.active = true;
    AirbandAutoView gd = idle; gd.freqHz = 121.5e6;
    st = a.step(5 + AirbandAuto::kKeepSec + 1, {}, {idle, act, gd}, 117.9e6, 137.1e6);
    CHECK(st.drop.size() == 1 && st.drop[0] == 125.0e6, "only the idle one goes: %zu", st.drop.size());
    st = a.step(10, {}, {idle}, 117.9e6, 137.1e6);
    CHECK(st.drop.empty(), "a recent one stays");
    // pinned (listen): goes when it never opened within 20 s
    AirbandAutoView pin = idle; pin.pinned = true; pin.lastEndSec = -1; pin.madeSec = 0;
    st = a.step(25, {}, {pin}, 117.9e6, 137.1e6);
    CHECK(st.drop.size() == 1, "an unheard listen request is dropped");
    // a full house: the quietest idle one makes room, a pinned one never does
    std::vector<AirbandAutoView> full;
    for (size_t i = 0; i < kAirbandAutoMax; i++) { AirbandAutoView x; x.freqHz = 120e6 + 25e3 * (double)i; x.madeSec = 0; x.lastEndSec = (double)i; full.push_back(x); }
    full[0].pinned = true;
    st = a.step(20, {seenAt(130.0e6, false, 20, 3)}, full, 124e6, 131e6);
    CHECK(st.add.size() == 1 && st.drop.size() == 1 && st.drop[0] == full[1].freqHz, "full: the quietest unpinned goes (%zu add, %zu drop)", st.add.size(), st.drop.size());
    // off: nothing opens, not even the guard
    a.setEnabled(false);
    st = a.step(10, {seenAt(118.5e6, false, 20, 9)}, {}, 117.9e6, 137.1e6);
    CHECK(st.add.empty(), "auto off adds nothing");
    a.setEnabled(true);
    // learning: a heard frequency opens on the first sighting; continuous marking; save and load
    a.noteTransmission(124.35e6, 6.0);
    st = a.step(10, {seenAt(124.35e6, false, 20, 1)}, {}, 123e6, 125e6);
    CHECK(st.add.size() == 1, "a learned frequency opens at once");
    a.markContinuous(127.0e6);
    CHECK(a.continuous(127.0e6) && !a.continuous(124.35e6), "continuous flag");
    const std::string blob = a.save();
    AirbandAuto b;
    CHECK(b.load(blob) && b.continuous(127.0e6) && b.score(124.35e6) > 1, "save and load: %s", blob.c_str());
    CHECK(!b.load("garbage"), "load refuses another format");
    a.noteTransmission(127.0e6, 4.0);
    CHECK(!a.continuous(127.0e6), "a short transmission clears the continuous flag");
    const auto d = airbandLearnedDecode("l1:124350000,3.00,2,0;bad;99,1,1,0;118500000,nan,1,0;125000000,5.50,1,1;");
    CHECK(d.size() == 2 && d[1].continuous, "decode keeps good records only: %zu", d.size());
}

static void hopper() {
    AirbandHopper h;
    h.plan(2.0e6);
    CHECK(h.groups() == 10 && h.needed(), "2 MHz captures: %zu groups", h.groups());
    for (size_t g = 0; g < h.groups(); g++) CHECK(h.hiOf(g) - h.loOf(g) <= 2.0e6 + 1, "group %zu fits", g);
    CHECK(std::fabs(h.loOf(0) - 118e6) < 1 && std::fabs(h.hiOf(h.groups() - 1) - 137e6) < 1, "tiles the band");
    AirbandHopper one; one.plan(25e6);
    CHECK(one.groups() == 1 && !one.needed() && one.tick(100, false, 120e6) < 0, "a wide radio does not hop");
    // simulate 900 s: group 3 is busy (a transmission on arrival, then quiet), others dead
    std::vector<int> visits(h.groups(), 0);
    std::vector<double> last(h.groups(), 0), maxGap(h.groups(), 0);
    h.locate(h.centerOf(0), 0);
    visits[0] = 1;
    int hops = 0;
    for (double t = 0; t < 900; t += 0.1) {
        const int g = h.tick(t, false, h.centerOf(h.current()));
        if (g >= 0) {
            hops++;
            CHECK((size_t)g != h.current() || h.groups() < 2, "moves to another capture");
            h.locate(h.centerOf((size_t)g), t);
            visits[(size_t)g]++;
            maxGap[(size_t)g] = std::max(maxGap[(size_t)g], t - last[(size_t)g]);
            last[(size_t)g] = t;
            if (g == 3) h.noteActivity(0.5 * (h.loOf(3) + h.hiOf(3)), t);
        }
    }
    int others = 0, worst = 0;
    for (size_t g = 0; g < h.groups(); g++) if (g != 3) { others = std::max(others, visits[g]); if (visits[g] == 0) worst++; }
    CHECK(worst == 0, "every capture is checked at least once");
    CHECK(visits[3] > others, "the busy capture is revisited more often: %d against at most %d", visits[3], others);
    for (size_t g = 0; g < h.groups(); g++) if (g != 3) CHECK(maxGap[g] <= 200, "capture %zu rechecked within 200 s (gap %.0f)", g, maxGap[g]);
    printf("  hopper: %d hops in 900 s, busy capture %d visits, quiet ones %d at most\n", hops, visits[3], others);
    // it stays while a transmission is open (up to the limit), then goes on
    AirbandHopper s; s.plan(2.0e6);
    s.locate(s.centerOf(2), 0);
    bool moved = false;
    for (double t = 0; t < 30; t += 0.1) if (s.tick(t, true, s.centerOf(2)) >= 0) moved = true;
    CHECK(!moved, "stays while open");
    int g = -1;
    for (double t = 30; t < 40 && g < 0; t += 0.1) g = s.tick(t, false, s.centerOf(2));
    CHECK(g >= 0, "goes on after it closed");
    // a retune that never arrives falls back to where the radio is
    AirbandHopper f; f.plan(2.0e6);
    f.locate(f.centerOf(0), 0);
    int asked = -1;
    double t = 0;
    for (; t < 10 && asked < 0; t += 0.1) asked = f.tick(t, false, f.centerOf(0));
    CHECK(asked >= 0, "asks to move");
    for (const double end = t + 3.2; t < end; t += 0.1) f.tick(t, false, f.centerOf(0));
    CHECK(f.current() == 0, "no retune: the current capture is where the radio is (%zu)", f.current());
}

// ---------------------------------------------------------------- the receiver

static const double kDial = 121.500e6;
static const double kOff = airbandTuning().tuneOffsetHz;

struct Car { double off; bool n833; int audio; std::vector<std::pair<double, double>> bursts; double level; };

static AirbandGenConfig gcfg(double rate, double cfo, double snr, const std::vector<Car>& cars) {
    AirbandGenConfig g; g.rate = rate; g.dialOffsetHz = -kOff; g.snrDb = snr; g.cfoHz = cfo;
    for (const auto& c : cars) {
        AirbandGenChannel x; x.offsetHz = c.off; x.is833 = c.n833; x.audio = c.audio; x.bursts = c.bursts; x.levelDb = c.level; x.mod = 0.8;
        g.chans.push_back(x);
    }
    return g;
}

static AirbandTelemetry runRx(const AirbandGenConfig& g, double secs, const std::function<void(AirbandReceiver&)>& setup, const std::function<void(AirbandReceiver&, double)>& each = nullptr) {
    AirbandGenerator gen(g);
    AirbandReceiver rx;
    rx.setSilent(true);
    rx.configure(g.rate);
    rx.setCenterHz(kDial + kOff);
    setup(rx);
    std::vector<cf32> x(8192);
    for (double done = 0; done < secs * g.rate; done += (double)x.size()) {
        gen.generate(x.data(), x.size());
        rx.feed(x.data(), x.size());
        if (each) each(rx, done / g.rate);
    }
    AirbandTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

static void receiver() {
    // five carriers: 25 kHz and 8.33 kHz raster (two neighbours), each keyed twice with 3 s between, voice-like audio
    const std::vector<Car> cars = {
        {0, false, 0, {{1.0, 2.0}, {6.0, 2.0}}, 0},                  // 121.500 (the guard, also keyed)
        {-150000, false, 0, {{2.0, 2.0}, {7.0, 2.0}}, -3},           // 121.350
        {100000, false, 0, {{3.0, 2.0}, {8.0, 2.0}}, -6},            // 121.600
        {200000 + 8333.3333, true, 0, {{1.5, 2.0}, {6.5, 2.0}}, -3},       // 8.33 raster
        {-250000 + 16666.667, true, 0, {{4.0, 2.0}, {9.0, 2.0}}, -6}};     // 8.33 raster
    std::set<std::string> want;
    for (const auto& c : cars) want.insert(airbandName(kDial + c.off, c.n833));
    for (double cfo : {0.0, 5000.0}) {
        const AirbandTelemetry t = runRx(gcfg(1.0e6, cfo, 25, cars), 12, [](AirbandReceiver& r) { r.setAuto(true); r.setChannels({}); });
        std::set<std::string> got;
        for (const auto& e : t.activity) got.insert(e.name);
        for (const auto& n : want) CHECK(got.count(n), "radio %+.0f Hz: %s was not opened by auto (heard: %zu names)", cfo, n.c_str(), got.size());
        for (const auto& n : got) CHECK(want.count(n), "radio %+.0f Hz: %s opened but nobody is there", cfo, n.c_str());
        int autos = 0; bool guard = false;
        for (const auto& c : t.channels) { autos += c.autoAdded; if (c.label == "guard") guard = true; }
        CHECK(t.autoMode && autos >= 1 && guard == true, "radio %+.0f Hz: %d auto channels, guard %d", cfo, autos, (int)guard);
        printf("  auto, radio %+.0f Hz: %zu of %zu carriers opened, %zu transmissions logged\n", cfo, got.size(), want.size(), t.activity.size());
        CHECK(t.activity.size() >= 7, "radio %+.0f Hz: %zu transmissions", cfo, t.activity.size());
    }
    // the guard is open and has priority when nothing transmits
    {
        const AirbandTelemetry t = runRx(gcfg(1.0e6, 0, 20, {}), 4, [](AirbandReceiver& r) { r.setAuto(true); r.setChannels({}); });
        bool g = false;
        for (const auto& c : t.channels) if (c.label == "guard" && c.autoAdded && c.priority && c.inBand) g = true;
        CHECK(g && t.activity.empty(), "guard open with priority, nothing else (%zu channels, %zu transmissions)", t.channels.size(), t.activity.size());
    }
    // auto off (the receiver's default): exactly as before, no channel appears
    {
        const AirbandTelemetry t = runRx(gcfg(1.0e6, 0, 25, cars), 8, [](AirbandReceiver& r) { r.setChannels({}); });
        CHECK(t.channels.empty() && t.activity.empty() && !t.autoMode, "auto off: %zu channels", t.channels.size());
        CHECK(!t.seen.empty(), "the band watch still lists what is on the air");
    }
    // listen to a carrier that is not in the list, auto off: it opens and is heard
    {
        AirbandGenConfig g = gcfg(1.0e6, 0, 25, {cars[1]});
        g.chans[0].bursts = {{3.0, 3.0}};
        const AirbandTelemetry t = runRx(g, 8, [](AirbandReceiver& r) { r.setChannels({}); }, [&](AirbandReceiver& r, double now) { if (now > 0.4 && now < 0.45) r.listenTo(kDial - 150000, false); });
        bool heard = false;
        for (const auto& e : t.activity) heard |= e.name == airbandName(kDial - 150000, false);
        CHECK(heard, "listenTo opened the carrier (%zu transmissions)", t.activity.size());
        bool off = false;
        for (const auto& c : t.channels) off |= c.autoAdded && c.label == "listen";
        CHECK(off && !t.autoMode, "the listen channel is a pinned extra channel");
    }
    // a listen request outside the capture is refused quietly
    {
        const AirbandTelemetry t = runRx(gcfg(1.0e6, 0, 25, {}), 2, [](AirbandReceiver& r) { r.setChannels({}); }, [&](AirbandReceiver& r, double now) { if (now > 0.3 && now < 0.35) r.listenTo(130e6, false); });
        CHECK(t.channels.empty(), "outside the capture: nothing opened");
    }
    // the user's list stays first and unchanged, auto channels come after
    {
        AirbandChannel c; c.freqHz = kDial - 150000; c.label = "Mine";
        const AirbandTelemetry t = runRx(gcfg(1.0e6, 0, 25, cars), 10, [&](AirbandReceiver& r) { r.setAuto(true); r.setChannels({c}); });
        CHECK(t.channels.size() >= 2 && t.channels[0].label == "Mine" && !t.channels[0].autoAdded && t.channels[1].autoAdded, "list first, auto after");
        int mine = 0;
        for (const auto& e : t.activity) if (e.chan == 0) mine++;
        CHECK(mine >= 2, "the listed channel keeps working: %d", mine);
        for (const auto& ch : t.channels) if (ch.autoAdded) CHECK(std::fabs(ch.freqHz - c.freqHz) > 600, "no duplicate of the listed channel");
    }
    // hopping: with a narrow radio, auto and an empty list, the receiver keeps asking for the next capture inside the band
    {
        uint32_t last = 0; int asks = 0; bool ok = true;
        const AirbandTelemetry t = runRx(gcfg(1.0e6, 0, 20, {}), 30, [](AirbandReceiver& r) { r.setAuto(true); r.setHop(true); r.setChannels({}); },
            [&](AirbandReceiver& r, double) {
                AirbandTelemetry q; r.telemetry(q, 0);
                if (q.hopSeq != last) { last = q.hopSeq; asks++; ok &= q.hopTargetHz >= 118e6 && q.hopTargetHz <= 137e6; }
            });
        CHECK(t.hopGroups > 5 && asks >= 3 && ok, "hopping: %d captures, %d moves asked", t.hopGroups, asks);
        const AirbandTelemetry nl = runRx(gcfg(1.0e6, 0, 20, {}), 12, [](AirbandReceiver& r) { r.setAuto(true); r.setHop(true); AirbandChannel c; c.freqHz = kDial; r.setChannels({c}); });
        CHECK(nl.hopSeq == 0, "no hopping while there is a list (%u)", nl.hopSeq);
    }
    // a receiver restarted with what it learned seeds from it
    {
        AirbandReceiver rx;
        AirbandLearned l; l.freqHz = 124.35e6; l.score = 8; l.opens = 4;
        rx.setLearnedState(airbandLearnedEncode({l}));
        CHECK(rx.learnedState().compare(0, 3, "l1:") == 0, "learned state kept");
        rx.forgetLearned();
        CHECK(rx.learnedState() == "l1:", "forget");
    }
}

int main() {
    logic();
    hopper();
    receiver();
    return testSummary("airband auto");
}
