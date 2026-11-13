// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Accuracy of the landing prediction on simulated flights (tests/sonde/sonde_predict_sim.h, docs/SONDE-PREDICT-4.0.md): the error of the
// old predictor (3.9, as the app used it: ground at sea level; and with the true ground) and the new one (offline; with a forecast) when
// the sonde is 10 km, 5 km, 2 km, 1 km, 500 m and 200 m above the ground, and at the last frame heard. Prints the table; checks that
// the new predictor beats the old one and that its 50 % / 95 % radii hold about that share of the true landings.
// ONAIR_SONDE_TUNE=1 also prints a small sweep of the drift correlation length and the wind change rate.
#include "sonde_predict_sim.h"
#include "dect2/sonde/sonde_land.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "../common/test_util.h"
using namespace dect2;

namespace {
const double kHeights[] = {10000, 5000, 2000, 1000, 500, 200};
constexpr int kNH = 7;   // the six heights and the last frame
const char* kHName[kNH] = {"10 km", "5 km", "2 km", "1 km", "500 m", "200 m", "last frame"};
enum { OldApp, OldGround, NewOff, NewFc, kNP };
const char* kPName[kNP] = {"old (3.9 app, ground 0)", "old (true ground)", "new, offline", "new + forecast"};

struct Acc { std::vector<double> e[kNP][kNH]; int in50 = 0, in95 = 0, cov = 0; std::vector<double> r50[kNH], r95[kNH]; int c50[kNH] = {}, c95[kNH] = {}, cn[kNH] = {}; };

double pct(std::vector<double> v, double p) {
    if (v.empty()) return NAN;
    std::sort(v.begin(), v.end());
    return v[(size_t)std::min<double>(v.size() - 1, std::floor(p * (v.size() - 1) + 0.5))];
}

SondePredictOpts gOpts;

void runFlight(const sim::Flight& F, unsigned seed, Acc& A, int members) {
    SondeLandTracker tr;
    sim::ThinTrack thin;
    const auto fc = F.forecast(2.0, seed + 99);
    SondePredictEnv envOff, envFc;
    // the terrain model given to the predictor: the truth plus a 5 m error that varies over a few hundred metres
    auto dem = [&F](double la, double lo, double& h) { h = F.ground.at(la, lo) + 5 * std::sin(la * 3000) * std::cos(lo * 2000); return true; };
    envOff.ground = dem; envFc.ground = dem; envFc.forecast = &fc;
    int next = 0;
    const size_t nf = F.frames.size();
    for (size_t i = 0; i < nf; i++) {
        const sim::Frame& f = F.frames[i];
        tr.addFix(f.t, f.lat, f.lon, f.alt, f.tempC);
        thin.add(f);
        if (!f.desc) continue;
        int slot = -1;
        while (next < 6 && f.trueAgl < kHeights[next]) { slot = next; next++; }
        const bool lastFrame = i + 1 == nf;
        if (slot < 0 && !lastFrame) continue;
        if (slot >= 0 && kHeights[slot] - f.trueAgl > 400) slot = -1;   // first heard well below that height (missed ascent)
        for (int s : {slot, lastFrame ? 6 : -1}) {
            if (s < 0) continue;
            SondeLandOpts lo;
            const SondeLanding a = predictSondeLandingTrack(thin.tr, f.lat, f.lon, f.alt, f.vz, f.hs, f.hd, lo, 0);
            const SondeLanding b = predictSondeLandingTrack(thin.tr, f.lat, f.lon, f.alt, f.vz, f.hs, f.hd, lo, F.landGround);
            SondePredictOpts o = gOpts; o.members = 0;
            const SondePrediction c = tr.predict(o, envOff);
            SondePredictOpts o2 = gOpts; o2.members = members; o2.seed = seed;
            const SondePrediction d = tr.predict(o2, envFc);
            const SondeLanding* L[2] = {&a, &b};
            for (int p = 0; p < 2; p++) if (L[p]->valid) A.e[p][s].push_back(sim::errM(L[p]->lat, L[p]->lon, F.landLat, F.landLon));
            if (c.valid) A.e[NewOff][s].push_back(sim::errM(c.lat, c.lon, F.landLat, F.landLon));
            if (d.valid) {
                const double e = sim::errM(d.lat, d.lon, F.landLat, F.landLon);
                A.e[NewFc][s].push_back(e);
                if (d.hasSpread) { A.cn[s]++; A.c50[s] += e <= d.r50M; A.c95[s] += e <= d.r95M; A.r50[s].push_back(d.r50M); A.r95[s].push_back(d.r95M); }
            }
        }
    }
}

void printTable(const char* title, const Acc& A) {
    printf("\n%s: median / 90th percentile error, metres (flights)\n", title);
    printf("%-26s", "sonde height above ground");
    for (int h = 0; h < kNH; h++) printf("| %-17s", kHName[h]);
    printf("\n");
    for (int p = 0; p < kNP; p++) {
        printf("%-26s", kPName[p]);
        for (int h = 0; h < kNH; h++) {
            if (A.e[p][h].empty()) { printf("| %-17s", "-"); continue; }
            char b[40]; snprintf(b, sizeof b, "%.0f / %.0f (%zu)", pct(A.e[p][h], 0.5), pct(A.e[p][h], 0.9), A.e[p][h].size());
            printf("| %-17s", b);
        }
        printf("\n");
    }
    printf("%-26s", "new: truth inside r50/r95");
    for (int h = 0; h < kNH; h++) {
        if (!A.cn[h]) { printf("| %-17s", "-"); continue; }
        char b[40]; snprintf(b, sizeof b, "%.0f%% / %.0f%%", 100.0 * A.c50[h] / A.cn[h], 100.0 * A.c95[h] / A.cn[h]);
        printf("| %-17s", b);
    }
    printf("\n%-26s", "new: median r50 / r95");
    for (int h = 0; h < kNH; h++) {
        if (!A.cn[h]) { printf("| %-17s", "-"); continue; }
        char b[40]; snprintf(b, sizeof b, "%.0f / %.0f", pct(A.r50[h], 0.5), pct(A.r95[h], 0.5));
        printf("| %-17s", b);
    }
    printf("\n");
}

void merge(Acc& T, const Acc& A) {
    for (int p = 0; p < kNP; p++) for (int h = 0; h < kNH; h++) T.e[p][h].insert(T.e[p][h].end(), A.e[p][h].begin(), A.e[p][h].end());
    for (int h = 0; h < kNH; h++) {
        T.cn[h] += A.cn[h]; T.c50[h] += A.c50[h]; T.c95[h] += A.c95[h];
        T.r50[h].insert(T.r50[h].end(), A.r50[h].begin(), A.r50[h].end()); T.r95[h].insert(T.r95[h].end(), A.r95[h].begin(), A.r95[h].end());
    }
}
}  // namespace

int main() {
    struct Set { const char* name; sim::Scenario sc; int n; };
    std::vector<Set> sets;
    { sim::Scenario s; sets.push_back({"normal flights (hills, lost 150..1200 m up)", s, 60}); }
    { sim::Scenario s; s.lastAglMin = 20; s.lastAglMax = 120; sets.push_back({"heard nearly to the ground (20..120 m)", s, 25}); }
    { sim::Scenario s; s.front = true; sets.push_back({"low-level wind changing fast (front)", s, 20}); }
    { sim::Scenario s; s.missAscent = true; sets.push_back({"ascent missed (first heard 8..16 km on the way down)", s, 20}); }
    { sim::Scenario s; s.noChute = true; sets.push_back({"no parachute (14..22 m/s at sea level)", s, 12}); }
    Acc total;
    unsigned seed = 1000;
    for (const Set& S : sets) {
        Acc A;
        for (int k = 0; k < S.n; k++, seed++) runFlight(sim::makeFlight(seed, S.sc), seed, A, 100);
        printTable(S.name, A);
        merge(total, A);
    }
    printTable("ALL simulated flights", total);
    // checks: the new predictor is better than the old one with the true ground at every height, the forecast does not hurt overall,
    // and the 95 % radius holds most landings (not a guarantee, the test of the error model)
    for (int h = 0; h < kNH; h++) {
        const double ob = pct(total.e[OldGround][h], 0.5), nf = pct(total.e[NewFc][h], 0.5), no = pct(total.e[NewOff][h], 0.5);
        CHECK(nf < ob, "%s: new + forecast %.0f m vs old %.0f m", kHName[h], nf, ob);
        CHECK(no < ob * 1.1, "%s: new offline %.0f m vs old %.0f m", kHName[h], no, ob);
        if (total.cn[h] > 20) {
            const double c95 = (double)total.c95[h] / total.cn[h], c50 = (double)total.c50[h] / total.cn[h];
            CHECK(c95 > 0.75, "%s: only %.0f %% inside r95", kHName[h], 100 * c95);
            CHECK(c50 > 0.25 && c50 < 0.85, "%s: %.0f %% inside r50", kHName[h], 100 * c50);
        }
    }
    if (const char* t = std::getenv("ONAIR_SONDE_TUNE"); t && *t == '1') {
        printf("\ntuning (normal flights, 30 per row): median error new + forecast, metres\n%-34s| 2 km | 1 km | 500 m | 200 m | last\n", "");
        for (double lc : {300.0, 600.0, 1200.0, 3000.0})
            for (double wc : {0.5, 1.0, 2.0}) {
                gOpts = SondePredictOpts(); gOpts.driftCorrLenM = lc; gOpts.windChangeMsPerH = wc;
                Acc A; sim::Scenario s;
                for (int k = 0; k < 30; k++) runFlight(sim::makeFlight(5000 + k, s), 5000 + k, A, 0);
                printf("Lc %5.0f m, change %.1f m/s/h      ", lc, wc);
                for (int h = 2; h < kNH; h++) printf("| %5.0f", pct(A.e[NewFc][h], 0.5));
                printf("\n");
            }
        for (double win : {30.0, 60.0, 90.0, 120.0}) {
            gOpts = SondePredictOpts(); gOpts.approachWindowS = win;
            Acc A; sim::Scenario s;
            for (int k = 0; k < 30; k++) runFlight(sim::makeFlight(5000 + k, s), 5000 + k, A, 0);
            printf("approach window %3.0f s             ", win);
            for (int h = 2; h < kNH; h++) printf("| %5.0f", pct(A.e[NewFc][h], 0.5));
            printf("\n");
        }
    }
    return testSummary("sonde_predict_harness");
}
