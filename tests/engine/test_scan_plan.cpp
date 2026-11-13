// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The merged Scanner view's pure logic (app/scan/scan_plan.h): scope, scan list building, next channel, lockout, priority, hold, hit records, tone text.
#include "../../app/scan/scan_plan.h"
#include <cstdio>

using namespace scanplan;

static int gFail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); gFail++; } } while (0)

static Row R(double mhz, const char* group = "", bool lock = false, bool star = false) { Row r; r.mhz = mhz; r.mode = "fm"; r.group = group; r.lockout = lock; r.star = star; return r; }

int main() {
    std::vector<Row> rows = {R(100, "a"), R(101, "b", true), R(102, "a"), R(103, "b", false, true), R(0, "a")};
    Params p;
    CHECK(pick(rows, p) == (std::vector<int>{0, 2, 3}));            // locked out and zero-frequency rows are left out
    p.skipLocked = false;
    CHECK(pick(rows, p) == (std::vector<int>{0, 1, 2, 3}));
    p.skipLocked = true; p.scope = Scope::Group; p.group = "b";
    CHECK(pick(rows, p) == (std::vector<int>{3}));
    p.scope = Scope::Range; p.loMhz = 101.5; p.hiMhz = 103;
    CHECK(pick(rows, p) == (std::vector<int>{2, 3}));
    p.scope = Scope::Fast;
    CHECK(pick(rows, p).empty());                                   // the FFT fast scan keeps its own list
    p.scope = Scope::Starred;
    CHECK(pick(rows, p) == (std::vector<int>{3}));

    {   // entries: dwell default / own, one priority only
        rows[0].dwellSec = 5; rows[2].priority = true; rows[3].priority = true;
        Params q; std::vector<int> rowOf;
        auto e = toEntries(rows, q, rowOf);
        CHECK(e.size() == 3 && rowOf == (std::vector<int>{0, 2, 3}));
        CHECK(e[0].dwellSec == 5 && e[1].dwellSec == 2);
        CHECK(!e[0].priority && e[1].priority && !e[2].priority);
        q.priorityCheck = false; e = toEntries(rows, q, rowOf);
        CHECK(!e[0].priority && !e[1].priority && !e[2].priority);
    }
    {   // next channel: wraps, skips locked, -1 when all locked
        std::vector<int> order = {0, 1, 2};
        std::vector<Row> v = {R(1), R(2, "", true), R(3)};
        CHECK(nextChannel(v, order, -1, true) == 0);
        CHECK(nextChannel(v, order, 0, true) == 2);
        CHECK(nextChannel(v, order, 2, true) == 0);
        CHECK(nextChannel(v, order, 0, false) == 1);
        v[0].lockout = v[2].lockout = true;
        CHECK(nextChannel(v, order, 0, true) == -1);
    }
    CHECK(holdExpired(10, 0, 10) && !holdExpired(9.9, 0, 10) && !holdExpired(1000, 0, 0));
    CHECK(resumeDue(5, 2, 3) && !resumeDue(4.9, 2, 3));
    CHECK(priorityDue(5, 0, 5, true) && !priorityDue(5, 0, 5, false) && !priorityDue(500, 0, 0, true));
    {   // hit record keeps an earlier tone when none was read
        Row r; recordHit(r, 100, "88.5 Hz"); recordHit(r, 200);
        CHECK(r.hits == 2 && r.lastHit == 200 && r.tone == "88.5 Hz");
        CHECK(toneDiffers(r, "100.0 Hz") && !toneDiffers(r, "88.5 Hz") && !toneDiffers(r, ""));
    }
    CHECK(rangeSteps(118, 118.1, 25).size() == 5 && rangeSteps(1, 0, 25).empty() && rangeSteps(1, 2, 0).empty() && rangeSteps(0, 1000, 1, 100).size() == 100);
    CHECK(toneText(88.5, -1, false) == "88.5 Hz");
    CHECK(toneText(0, 23, false) == "D023N" && toneText(0, 645, true) == "D645I" && toneText(0, -1, false).empty());
    {   // storage round trip, defaults leave no keys
        Row r = R(1); std::map<std::string, std::string> o;
        writeRow(o, r); CHECK(o.empty());
        r.lockout = r.priority = true; r.hits = 3; r.lastHit = 123456; r.tone = "D023N";
        writeRow(o, r);
        Row b; readRow(o, b);
        CHECK(b.lockout && b.priority && b.hits == 3 && b.lastHit == 123456 && b.tone == "D023N");
        r.lockout = false; r.tone.clear(); writeRow(o, r);
        CHECK(o.count(kKeyLock) == 0 && o.count(kKeyTone) == 0 && o.count(kKeyHits) == 1);
    }
    {   // one priority row
        std::vector<Row> v = {R(1), R(2), R(3)}; v[0].priority = true;
        setPriority(v, 2, true); CHECK(!v[0].priority && v[2].priority);
        setPriority(v, 2, false); CHECK(!v[2].priority);
    }
    {   // hold limit in the machine: a channel that stays active is left after holdMaxSec
        scanlist::Machine mm;
        scanlist::Entry a, b; a.mhz = 1; b.mhz = 2; a.mode = b.mode = "fm";
        mm.list = {a, b}; mm.holdMaxSec = 5; mm.settleSec = 0; mm.hangSec = 1;
        double t = 0; mm.start(t);
        int tunes = 0;
        for (; t < 4; t += 0.1) if (mm.tick(t, true, true)) tunes++;
        CHECK(mm.state() == scanlist::Machine::Hold && tunes == 0);
        for (; t < 6; t += 0.1) if (mm.tick(t, true, true)) tunes++;
        CHECK(tunes >= 1 && mm.target().mhz == 2);
        scanlist::Machine nm;
        nm.list = {a, b}; nm.settleSec = 0;
        t = 0; nm.start(t); tunes = 0;
        for (; t < 60; t += 0.1) if (nm.tick(t, true, true)) tunes++;
        CHECK(tunes == 0);
    }
    if (!gFail) printf("scan_plan: all passed\n");
    return gFail ? 1 : 0;
}
