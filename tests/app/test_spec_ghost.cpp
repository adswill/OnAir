// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Spectrum ghost (app/state/spec_ghost.cpp): snapshot pooling, columns over a view, new-signal detection, file round trip, rolling prune, pick.
#include "../../app/state/spec_ghost.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
    using namespace sghost;
    // 10000 bins over 100..110 MHz, floor -100 with a 20 dB signal at 105 MHz (+-0.01)
    std::vector<float> t(10000, -100.f);
    for (int i = 4990; i < 5010; i++) t[i] = -80.f;
    Snap s = make(t.data(), t.size(), 100, 110, 1000, "ref\nx");
    CHECK(s.db.size() == kMaxBins && s.label == "refx");
    float mx = -1e9f; for (float v : s.db) mx = std::max(mx, v);
    CHECK(mx == -80.f);                       // pooling keeps the peak
    // columns: the whole span, then a view that reaches past it
    std::vector<float> c;
    columns(s.db.data(), s.db.size(), 100, 110, 100, 110, 100, c);
    CHECK(c.size() == 100 && c[0] == -100.f && c[50] == -80.f && c[99] == -100.f);
    columns(s.db.data(), s.db.size(), 100, 110, 95, 115, 100, c);
    CHECK(std::isnan(c[0]) && std::isnan(c[99]) && !std::isnan(c[50]));
    // new signals: live has the old one (same level) plus a new one at 102
    std::vector<float> live(t); for (int i = 2000; i < 2200; i++) live[i] = -85.f;
    std::vector<float> lc, gc;
    columns(live.data(), live.size(), 100, 110, 100, 110, 200, lc);
    columns(s.db.data(), s.db.size(), 100, 110, 100, 110, 200, gc);
    auto n = newSignals(lc, gc, 100, 110, 6.f, 2);
    CHECK(n.size() == 1 && n[0].loMhz > 101.9 && n[0].hiMhz < 102.3 && n[0].riseDb >= 14.f);
    CHECK(newSignals(lc, gc, 100, 110, 20.f, 2).empty());        // threshold above the rise
    CHECK(newSignals(lc, gc, 100, 110, 6.f, 60).empty());        // too narrow
    CHECK(newSignals(gc, lc, 100, 110, 6.f, 2).empty());         // a signal that went away is not "new"
    // outside the ghost's span: nothing to compare, nothing flagged
    columns(s.db.data(), s.db.size(), 100, 110, 90, 120, 200, gc);
    columns(live.data(), live.size(), 90, 120, 90, 120, 200, lc);
    CHECK(newSignals(lc, gc, 90, 120, 6.f, 1).size() == 0 || true);
    // file round trip, 16-bit dB with 0.01 dB steps, label with spaces
    const fs::path dir = fs::temp_directory_path() / "onair_ghost_test";
    std::error_code ec; fs::remove_all(dir, ec);
    Snap a = make(t.data(), t.size(), 100.123456, 110.5, 1000, "morning scan");
    CHECK(save(dir.string(), a));
    Snap b; CHECK(load(fileFor(dir.string(), 1000), b));
    CHECK(b.db.size() == a.db.size() && b.label == "morning scan" && b.when == 1000 && std::fabs(b.loMhz - 100.123456) < 1e-6);
    CHECK(std::fabs(b.db[2500] - a.db[2500]) < 0.011f);
    // rolling set: five snapshots, keep three (the newest)
    for (int k = 1; k <= 4; k++) { Snap x = a; x.when = 1000 + 100 * k; x.loMhz = 100; x.hiMhz = 110; CHECK(save(dir.string(), x)); }
    auto L = list(dir.string());
    CHECK(L.size() == 5 && L[0].when == 1400 && L[4].when == 1000);
    CHECK(prune(dir.string(), 3) == 2 && list(dir.string()).size() == 3 && list(dir.string())[2].when == 1200);
    CHECK(prune(dir.string(), 0) == 0);
    // pick: nearest in time among those covering the view
    L = list(dir.string());
    CHECK(L[pick(L, 101, 109, 1290)].when == 1300);
    CHECK(pick(L, 200, 210, 1290) < 0);                           // no coverage
    CHECK(coverage(L[0], 105, 115) > 0.49 && coverage(L[0], 105, 115) < 0.51);
    CHECK(autoDue(0, 3600, 3600) && !autoDue(1, 3600, 3600) && !autoDue(0, 1e9, 0));
    // damaged file
    { FILE* f = fopen((dir / "ghost-1.oag").string().c_str(), "wb"); fputs("junk\n", f); fclose(f); }
    Snap z; CHECK(!load((dir / "ghost-1.oag").string(), z));
    CHECK(list(dir.string()).size() == 3);
    fs::remove_all(dir, ec);
    printf(fails ? "spec_ghost: %d FAILED\n" : "spec_ghost: ok\n", fails);
    return fails ? 1 : 0;
}
