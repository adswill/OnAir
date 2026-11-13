// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The scanner's state machine (app/scan/scan_list.h) with a fake activity source: stops on activity, holds, hangs, resumes, lockout, priority, CSV.
#include "../../app/scan/scan_list.h"
#include <cmath>
#include <cstdio>
#include <functional>

using scanlist::Entry;
using scanlist::Machine;

static int gFail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); gFail++; } } while (0)

// Runs the machine in 0.1 s steps; active(mhz, t) is the fake activity source. Records each tune.
struct Sim {
    Machine m;
    double t = 0;
    std::vector<double> tunes;
    std::function<bool(double, double)> active = [](double, double) { return false; };
    void begin() { if (m.start(t)) tunes.push_back(m.target().mhz); }
    void run(double sec) {
        for (double end = t + sec; t < end - 1e-9; t += 0.1)
            if (m.tick(t, true, active(m.target().mhz, t))) tunes.push_back(m.target().mhz);
    }
};

static Entry E(double mhz, bool prio = false) { Entry e; e.mhz = mhz; e.mode = "fm"; e.dwellSec = 2; e.priority = prio; return e; }

int main() {
    {   // quiet list: steps through every entry, about one dwell (+ settle) each, and wraps
        Sim s; s.m.list = {E(100), E(101), E(102)};
        s.begin();
        s.run(7.5);
        CHECK(s.tunes.size() >= 4);
        CHECK(s.tunes[0] == 100 && s.tunes[1] == 101 && s.tunes[2] == 102 && s.tunes[3] == 100);
    }
    {   // stops on activity, holds while active, hangs 3 s, then resumes with the next entry
        Sim s; s.m.list = {E(100), E(101), E(102)};
        s.active = [](double mhz, double t) { return mhz == 101 && t < 10; };
        s.begin();
        s.run(3);                                 // 100 dwells 2.3 s, then 101
        CHECK(s.m.target().mhz == 101);
        s.run(1);
        CHECK(s.m.state() == Machine::Hold);
        CHECK(s.m.hitStarted());
        CHECK(!s.m.hitStarted());                 // once
        s.run(6);                                 // t = 10: still on 101
        CHECK(s.m.target().mhz == 101);
        s.run(1);                                 // activity ended at 10: hanging
        CHECK(s.m.state() == Machine::Hang && s.m.target().mhz == 101);
        s.run(2.5);                               // hang of 3 s is over at 13
        CHECK(s.m.target().mhz == 102);
        CHECK(s.m.state() == Machine::Dwell);
    }
    {   // activity during the hang time goes back to holding
        Sim s; s.m.list = {E(100), E(101)};
        s.active = [](double mhz, double t) { return mhz == 100 && (t < 2 || (t > 3.5 && t < 4)); };
        s.begin();
        s.run(3);
        CHECK(s.m.state() == Machine::Hang);
        s.run(1);
        CHECK(s.m.target().mhz == 100 && (s.m.state() == Machine::Hold || s.m.state() == Machine::Hang));
        s.run(3.2);                               // last activity 3.9 + 3 s
        CHECK(s.m.target().mhz == 101);
    }
    {   // activity in the settle time right after a tune is not believed
        Sim s; s.m.list = {E(100), E(101)};
        s.active = [](double, double t) { return t < 0.2; };
        s.begin();
        s.run(1);
        CHECK(s.m.state() == Machine::Dwell);
    }
    {   // lockout: the entry is skipped from then on; skip moves on without locking out
        Sim s; s.m.list = {E(100), E(101), E(102)};
        s.active = [](double mhz, double) { return mhz == 101; };
        s.begin();
        s.run(3);
        CHECK(s.m.target().mhz == 101 && s.m.state() == Machine::Hold);
        CHECK(s.m.lockout(s.t));
        CHECK(s.m.list[1].lockout);
        CHECK(s.m.target().mhz == 102);
        s.tunes.clear();
        s.run(10);
        for (double f : s.tunes) CHECK(f != 101);
        CHECK(s.m.skip(s.t));
        CHECK(s.m.state() == Machine::Dwell);
    }
    {   // band scan: steps, and a band lockout
        Sim s; s.m.useBand = true; s.m.band.startMhz = 118; s.m.band.stopMhz = 118.1; s.m.band.stepKhz = 25; s.m.band.mode = "airband";
        CHECK(s.m.bandSteps() == 5);
        s.active = [](double mhz, double) { return std::fabs(mhz - 118.05) < 1e-6; };
        s.begin();
        s.run(6);
        CHECK(std::fabs(s.m.target().mhz - 118.05) < 1e-6 && s.m.state() == Machine::Hold);
        CHECK(s.m.lockout(s.t));
        s.tunes.clear();
        s.run(15);
        for (double f : s.tunes) CHECK(std::fabs(f - 118.05) > 1e-6);
        CHECK(!s.tunes.empty());
    }
    {   // priority: checked every 5 s between entries, and while holding another channel (then back to it)
        Sim s; s.m.list = {E(100), E(101), E(102), E(150, true)};
        s.m.prioritySec = 5;
        s.begin();
        s.run(12);
        int prio = 0;
        for (double f : s.tunes) prio += f == 150;
        CHECK(prio >= 1);
        CHECK(s.tunes[0] == 100);                 // the priority entry is not in the normal rotation

        Sim h; h.m.list = {E(100), E(150, true)};
        h.m.prioritySec = 5;
        h.active = [](double mhz, double) { return mhz == 100; };
        h.begin();
        h.run(5.5);                               // holding 100, priority due at 5
        CHECK(h.m.target().mhz == 150);
        h.run(2.5);                               // quiet priority: back to the held channel
        CHECK(h.m.target().mhz == 100);
        h.run(1);
        CHECK(h.m.state() == Machine::Hold);

        Sim p; p.m.list = {E(100), E(150, true)};   // an active priority channel holds
        p.m.prioritySec = 5;
        p.active = [](double mhz, double t) { return mhz == 150 && t < 20; };
        p.begin();
        p.run(9);
        CHECK(p.m.target().mhz == 150 && p.m.state() == Machine::Hold);
    }
    {   // nothing to scan
        Machine m;
        CHECK(!m.start(0));
        m.list = {E(100)}; m.list[0].lockout = true;
        CHECK(!m.start(0));
    }
    {   // CSV round trip, with a comma and a quote in a name
        std::vector<Entry> v = {E(93.9), E(446.00625, true)};
        v[0].name = "Noor, \"Dubai\""; v[1].mode = "dpmr"; v[1].lockout = true; v[1].dwellSec = 4.5;
        const std::vector<Entry> w = scanlist::fromCsv(scanlist::toCsv(v));
        CHECK(w.size() == 2);
        if (w.size() == 2) {
            CHECK(std::fabs(w[0].mhz - 93.9) < 1e-9 && w[0].name == v[0].name && w[0].mode == "fm");
            CHECK(std::fabs(w[1].mhz - 446.00625) < 1e-9 && w[1].mode == "dpmr" && w[1].lockout && w[1].priority && w[1].dwellSec == 4.5);
        }
        CHECK(scanlist::fromCsv("junk\n\n145.5,fm\r\n").size() == 1);
    }
    printf(gFail ? "scanner: %d failures\n" : "scanner: ok\n", gFail);
    return gFail ? 1 : 0;
}
