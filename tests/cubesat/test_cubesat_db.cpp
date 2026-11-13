// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The satellite tables and pass planner: the snapshot loads, the planner finds passes for an observer in Lithuania, the Doppler shift has the
// right size and sign, user transmitters, merging newer TLEs.
#include "dect2/cubesat/cubesat_db.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
using namespace dect2;
using namespace dect2::cubesat;
#include "../common/test_util.h"

int main() {
    const std::vector<TxEntry>& t = txTable();
    CHECK(t.size() > 300, "transmitters %zu", t.size());
    std::set<int> sats;
    int cw = 0, afsk = 0, fsk = 0, bpsk = 0, lora = 0;
    for (const TxEntry& e : t) {
        sats.insert(e.norad);
        CHECK(e.hz >= 130e6 && e.hz <= 480e6, "%s frequency %.0f", e.name.c_str(), e.hz);
        cw += e.mod == ModCw; afsk += e.mod == ModAfsk; fsk += e.mod == ModFsk; bpsk += e.mod == ModBpsk; lora += e.mod == ModLora;
    }
    CHECK(sats.size() > 200 && cw > 20 && afsk > 10 && fsk > 100 && bpsk > 20 && lora > 5, "mix: %zu sats cw %d afsk %d fsk %d bpsk %d lora %d", sats.size(), cw, afsk, fsk, bpsk, lora);
    PassPlanner pl;
    Observer vilnius; vilnius.latDeg = 54.69; vilnius.lonDeg = 25.28; vilnius.altKm = 0.1;
    pl.setObserver(vilnius);
    CHECK(pl.satelliteCount() > 200, "TLEs parsed for %zu satellites", pl.satelliteCount());
    // the snapshot is from early November 2026: plan from its newest epoch, TLEs are good for days
    const double now = jdToUnix(pl.newestEpochJd()) + 3600;
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<PlannedPass> ps = pl.upcoming(now, 12, 10, 0);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("%zu passes above 10 deg in 12 h, planned in %.0f ms\n", ps.size(), ms);
    CHECK(ps.size() > 40, "passes %zu", ps.size());
    for (size_t i = 1; i < ps.size(); i++) CHECK(ps[i - 1].p.aos <= ps[i].p.aos, "sorted");
    for (const PlannedPass& p : ps) CHECK(p.p.los > p.p.aos && p.p.maxElDeg >= 10 && !p.tx.empty() && p.p.los - p.p.aos < 1800, "pass sane");
    // Doppler of a pass: positive (rising) shift before TCA, negative after, size about 10 kHz at 436 MHz (at most 12 kHz for 550 km)
    const PlannedPass* best = nullptr;
    for (const PlannedPass& p : ps) if (!best || p.p.maxElDeg > best->p.maxElDeg) best = &p;
    if (best) {
        double a = 0, b = 0, c = 0;
        CHECK(pl.doppler(best->norad, best->p.aos + 5, 436e6, a) && pl.doppler(best->norad, best->p.tca, 436e6, b) && pl.doppler(best->norad, best->p.los - 5, 436e6, c), "doppler");
        printf("%s max %.0f deg: Doppler at 436 MHz %.0f / %.0f / %.0f Hz\n", best->name.c_str(), best->p.maxElDeg, a, b, c);
        CHECK(a > 3000 && a < 14000 && c < -3000 && c > -14000 && std::fabs(b) < 3500, "Doppler sign and size");
    }
    // user transmitters
    std::vector<TxEntry> u = txFromText("25544|ISS test|145.8250|2|1|1200;bad;0|x|1|1|1|1;");
    CHECK(u.size() == 1 && u[0].norad == 25544 && std::fabs(u[0].hz - 145.825e6) < 1 && u[0].mod == ModAfsk, "user table parse");
    CHECK(txFromText(txToText(u)).size() == 1, "user table round trip");
    const size_t before = pl.transmitters().size();
    pl.setExtraTx(u);
    CHECK(pl.transmitters().size() == before + 1, "extra transmitters");
    // a TLE of a newer epoch replaces, an older one does not
    const std::string text = tleSnapshotText();
    CHECK(pl.addTleText(text) == 0, "same TLEs again change nothing");
    CHECK(pl.nameOf(40000).size() > 0, "name of an unknown satellite");
    return testSummary("cubesat db");
}
