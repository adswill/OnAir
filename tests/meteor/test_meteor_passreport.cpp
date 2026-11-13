// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pass report card (round 4 #20): a pass from the receiver's running counters (frames before it not counted, the end after a quiet minute,
// a second pass on continuing counters, a receiver restart resetting them), the comparison with similar passes, and the CSV.
#include "dect2/meteor/pass_report.h"
#include "../common/test_util.h"
#include <cmath>
using namespace dect2;

int main() {
    PassTracker t;
    const int64_t t0 = 1'800'000'000;
    PassReport r;
    // noise before the pass: some bad frames counted by the receiver
    for (int s = 0; s < 60; s++) t.update(t0 + s, 1, 2, 0, 5, 0, 0, 0);
    CHECK(!t.active(), "not before frames decode");
    // the pass: 600 s, 10 good frames a second, 1 bad, SNR 6 -> 10 -> 6 dB
    uint64_t ok = 0, bad = 5;
    for (int s = 60; s < 660; s++) { ok += 10; bad += 1; const float snr = 6 + 4 * (float)std::sin((s - 60) * 3.14159 / 600); t.update(t0 + s, 3, snr, ok, bad, 3, (s - 60) * 6, 40); }
    CHECK(t.active() && !t.finish(t0 + 690, r), "not over yet after 30 s quiet");
    for (int s = 660; s < 730; s++) t.update(t0 + s, 1, 1, ok, bad, 3, 3594, 40);
    CHECK(t.finish(t0 + 730, r) && !t.active(), "over after a quiet minute");
    CHECK(r.startUtc == t0 + 60 && r.framesOk == 6000 && r.framesBad == 600 && std::fabs(r.syncPct() - 90.909) < 0.01 && r.lines == 3594 && r.gapLines == 40 &&
          r.snrMaxDb > 9.9 && r.snrMeanDb > 8 && r.snrMeanDb < 9, "report: %llu ok, %.1f %%, snr %.1f / %.1f", (unsigned long long)r.framesOk, r.syncPct(), r.snrMeanDb, r.snrMaxDb);
    // a second pass on the same counters
    for (int s = 2000; s < 2300; s++) { ok += 5; bad += 5; t.update(t0 + s, 3, 3, ok, bad, 3, (s - 2000) * 6, 0); }
    PassReport r2;
    CHECK(t.finish(t0 + 2400, r2) && r2.framesOk == 1500 && r2.framesBad == 1500, "second pass counted from its own start (%llu)", (unsigned long long)r2.framesOk);
    // the receiver restarted: counters from 0
    for (int s = 3000; s < 3100; s++) t.update(t0 + s, 3, 5, (uint64_t)(s - 2999) * 2, 0, 0, 100, 0);
    PassReport r3;
    CHECK(t.finish(t0 + 3200, r3) && r3.framesOk == 200, "after a restart (%llu)", (unsigned long long)r3.framesOk);
    CHECK(!t.finish(t0 + 4000, r3, 60, true), "nothing to finish");
    // compare
    auto P = [](int64_t st, double el, int lines, uint64_t ok, uint64_t bad) { PassReport p; p.startUtc = st; p.satellite = "METEOR-M2 4"; p.maxElDeg = el; p.lines = lines; p.framesOk = ok; p.framesBad = bad; return p; };
    std::vector<PassReport> h = {P(1, 60, 3000, 900, 100), P(2, 65, 3200, 950, 50), P(3, 20, 800, 500, 500), P(4, 58, 3100, 900, 100)};
    PassCompare c = passCompare(h, P(10, 62, 3100, 920, 80));
    CHECK(c.similar == 3 && c.verdict == "as usual" && std::fabs(c.meanLines - 3100) < 1e-9, "as usual (%d similar)", c.similar);
    CHECK(passCompare(h, P(11, 61, 2000, 600, 400)).verdict == "worse than usual" && passCompare(h, P(12, 22, 1500, 900, 100)).verdict == "better than usual", "worse / better");
    CHECK(passCompare(h, P(13, 85, 3000, 1, 1)).similar == 0 && passCompare(h, P(14, -1, 1, 1, 1)).verdict.empty(), "nothing similar; elevation not known");
    PassReport other = P(15, 60, 3000, 1, 1); other.satellite = "NOAA 19";
    CHECK(passCompare(h, other).similar == 0, "other satellites are not compared");
    const auto back = passFromCsv(passCsv({r, r2}) + "junk\n");
    CHECK(back.size() == 2 && back[0].startUtc == r.startUtc && back[0].framesOk == 6000 && back[0].lines == 3594 && std::fabs(back[0].snrMaxDb - r.snrMaxDb) < 0.06, "CSV round trip");
    return testSummary("meteor_passreport");
}
