// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sparkline ring, "too slow" hysteresis, the signal verdict and the one-click fix ladder (app/ui/meter_logic.h).
#include "../../app/ui/meter_logic.h"
#include <cstdio>
using namespace meterlogic;
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
int main() {
    Spark sp;
    check(sp.count == 0 && sp.maxv() == 0, "empty spark");
    for (int i = 1; i <= 40; i++) sp.push((float)i);
    check(sp.count == 30 && sp.at(0) == 11.f && sp.at(29) == 40.f && sp.maxv() == 40.f, "ring keeps the last 30, oldest first");

    check(!overloaded(300, 4, 50, false) && overloaded(341, 4, 50, false) && overloaded(10, 4, 100, false) && overloaded(10, 4, 10, true), "overload rule");
    check(overloaded(86, 1, 0, false) && !overloaded(84, 1, 0, false) && !overloaded(50, 0, 0, false), "85% of the cores, 0 cores counts as 1");
    SlowState s;
    check(!s.update(400, 4, 0, false) && !s.update(400, 4, 0, false), "two seconds over: no warning yet");
    check(s.update(400, 4, 0, false), "third second over: slow");
    for (int i = 0; i < 4; i++) check(s.update(100, 4, 0, false), "stays slow while recovering");
    check(!s.update(100, 4, 0, false), "clears after five seconds under");
    SlowState t;   // a blip is reset by a calm second
    t.update(400, 4, 0, false); t.update(400, 4, 0, false); t.update(100, 4, 0, false);
    check(!t.update(400, 4, 0, false) && !t.update(400, 4, 0, false), "a calm second resets the count");
    SlowState m;
    for (int i = 0; i < 3; i++) m.update(400, 4, 0, false);
    for (int i = 0; i < 10; i++) m.update(320, 4, 0, false);   // 80% of 4 cores: between the two limits
    check(m.slow, "between 75% and 85% it does not clear");

    Signal g;
    check(classify(g) == Quality::None, "stopped");
    g.running = true; g.haveLevel = true; g.levelDb = -100;
    check(classify(g) == Quality::None, "below the floor");
    g.levelDb = -50;
    check(classify(g) == Quality::Weak, "level alone is Weak, never Locked");
    g.haveLock = true; g.locked = false;
    check(classify(g) == Quality::Weak, "no lock but a signal");
    g.locked = true;
    check(classify(g) == Quality::Locked, "locked");
    g.haveSnr = true; g.snrDb = 22;
    check(classify(g) == Quality::Good, "locked with SNR");
    g.snrDb = 9;
    check(classify(g) == Quality::Locked, "locked, low SNR");
    Signal h; h.running = true; h.haveLevel = true; h.levelDb = -40; h.meters = 3; h.anyGood = true; h.worst = 2;
    check(classify(h) == Quality::Locked, "meters: good and marginal");
    h.worst = 1; check(classify(h) == Quality::Good, "meters: all good");
    h.anyGood = false; h.worst = 3; check(classify(h) == Quality::Weak, "meters: bad only");
    check(qualitySegments(Quality::None) == 0 && qualitySegments(Quality::Weak) == 1 && qualitySegments(Quality::Locked) == 3 && qualitySegments(Quality::Good) == 4, "segments");
    check(std::string(qualityWord(Quality::None)) == "No signal" && std::string(qualityWord(Quality::Good)) == "Good", "words");

    check(nextWaterfallRate(0) == 5.f && nextWaterfallRate(10) == 5.f && nextWaterfallRate(5) == 2.f && nextWaterfallRate(2) == 1.f && nextWaterfallRate(1) == -1.f, "waterfall ladder");
    check(nextFft(32768) == 16384 && nextFft(8192) == 4096 && nextFft(4096) == 0 && nextFft(5000) == 4096, "fft ladder");

    float wf = 0; int fft = 32768;   // the ladder with fake settings: waterfall first, then FFT, then nothing
    auto steps = [&]() {
        std::vector<Step> v;
        v.push_back({"waterfall", [&] { const float n = nextWaterfallRate(wf); if (n < 0) return false; wf = n; return true; }, [&, old = wf] { wf = old; }});
        v.push_back({"fft", [&] { const int n = nextFft(fft); if (!n) return false; fft = n; return true; }, [&, old = fft] { fft = old; }});
        return v;
    };
    Relief r;
    check(r.fix(steps()) == "waterfall" && wf == 5.f && r.canUndo(), "first the waterfall");
    r.fix(steps()); r.fix(steps());   // 5 -> 2 -> 1
    check(wf == 1.f && fft == 32768, "waterfall stepped down");
    check(r.fix(steps()) == "fft" && fft == 16384, "then the FFT");
    while (!r.fix(steps()).empty()) {}
    check(fft == 4096 && r.fix(steps()).empty(), "nothing left");
    r.undoAll();
    check(!r.canUndo() && r.last().empty() && wf == 0.f && fft == 32768, "undo restores everything");
    if (!fails) printf("ok\n");
    return fails;
}
