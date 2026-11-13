// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Propagation model regression suite: synthetic known cases from public formulas and textbook values.
//  free space (P.525), two-ray / plane earth and the radio horizon, knife-edge (P.526), smooth-earth diffraction continuity, Okumura-Hata
//  open-area agreement, a broad P.1546-style band, troposcatter (P.452), clutter (P.2108), rain (P.838), gases, HF ground wave (P.368
//  behaviour), skywave MUF / absorption / skip zone / band closure / LUF against textbook ranges, P.372 noise and link-budget arithmetic.
#include "dect2/prop/prop.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
using namespace dect2::prop;
static const double kPi = 3.14159265358979323846;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b, tol) do { const double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (tol))) { printf("FAIL line %d: %s = %.4f, want %.4f +- %g\n", __LINE__, #a, _a, _b, (double)(tol)); fails++; } } while (0)
#define INRANGE(a, lo, hi) do { const double _a = (a); if (!(_a >= (lo) && _a <= (hi))) { printf("FAIL line %d: %s = %.4f, want %g..%g\n", __LINE__, #a, _a, (double)(lo), (double)(hi)); fails++; } } while (0)

// Okumura-Hata, open area (small/medium city mobile correction), 150..1500 MHz
static double hataOpen(double f, double hb, double hm, double d) {
    const double lf = std::log10(f);
    const double a = (1.1 * lf - 0.7) * hm - (1.56 * lf - 0.8);
    const double urban = 69.55 + 26.16 * lf - 13.82 * std::log10(hb) - a + (44.9 - 6.55 * std::log10(hb)) * std::log10(d);
    return urban - 4.78 * lf * lf + 18.33 * lf - 40.94;
}

static Params vhf(double f, double h1, double h2) {
    Params p; p.fMHz = f; p.txHeightM = h1; p.rxHeightM = h2; p.txPowerDbw = 10; p.txGainDbi = 0; p.env = Env::Quiet; p.atmosphericNoise = false;
    p.rxNfDb = 6; p.bandwidthHz = 12500; p.requiredSnrDb = 12; p.gas = false;
    p.pathModel = PathModel::Legacy39;   // this suite pins the 3.9 formulas; the 4.0 models are checked in itu40() and their own tests
    return p;
}

static void freeSpace() {
    NEAR(fsplDb(100, 10), 92.45, 0.01);
    NEAR(fsplDb(1000, 1), 92.45, 0.01);
    NEAR(fsplDb(2400, 1), 100.05, 0.02);
    NEAR(fsplDb(100, 20) - fsplDb(100, 10), 6.02, 0.01);
    NEAR(fsplDb(200, 10) - fsplDb(100, 10), 6.02, 0.01);
    // 1 kW ERP (EIRP 1 kW + 2.15 dB) at 1 km: 106.9 dBuV/m, the broadcast reference
    Params p = vhf(100, 10, 10);
    p.pathModel = PathModel::FreeSpace; p.txPowerDbw = 30; p.txGainDbi = 2.15;
    const Point a = evaluate(p, 1, 0);
    NEAR(a.fieldDbuvm, 106.9, 0.1);
    CHECK(a.model == Model::FreeSpace);
    NEAR(fieldDbuvm(-100, 100, 0), 17.2, 1e-9);
    NEAR(fieldDbuvm(-100, 100, 3), 14.2, 1e-9);
    NEAR(dbmToUv50(-107), 1.0, 0.01);   // 0 dBuV = -107 dBm in 50 ohms
}

static void twoRayAndHorizon() {
    NEAR(radioHorizonKm(10, 2), 4.12 * (std::sqrt(10.0) + std::sqrt(2.0)), 1e-9);
    NEAR(radioHorizonKm(0, 0), 0, 1e-12);
    NEAR(radioHorizonKm(10000, 10), 425, 3);   // an aircraft at 10 km sees about 425 km
    // plane earth meets free space at the breakpoint 4 pi h1 h2 / lambda
    const double lam = 299.792458 / 150.0, db = 4 * kPi * 30 * 10 / lam / 1000.0;
    NEAR(planeEarthLossDb(db, 30, 10), fsplDb(150, db), 0.05);
    NEAR(planeEarthLossDb(20, 30, 10) - planeEarthLossDb(10, 30, 10), 12.04, 0.01);   // 40 log d
    NEAR(planeEarthLossDb(10, 60, 10) - planeEarthLossDb(10, 30, 10), -6.02, 0.01);   // height gain 20 log h
    // exact two-ray: the first peak (path difference lambda / 2) is 6 dB above free space; far out it follows plane earth
    const double dpk = 4 * 30 * 10 / lam / 1000.0;
    NEAR(twoRayLossDb(150, dpk, 30, 10), fsplDb(150, dpk) - 6.02, 0.05);
    NEAR(twoRayLossDb(150, 40, 30, 10), planeEarthLossDb(40, 30, 10), 0.3);
    // Okumura-Hata open area agreement inside its validity range (Auto model, no troposcatter influence that close)
    for (double d : {2.0, 5.0, 10.0, 20.0}) {
        Params p = vhf(450, 50, 1.5);
        const Point q = evaluate(p, d, 0);
        if (std::fabs(q.pathLossDb - hataOpen(450, 50, 1.5, d)) > 6) printf("  Hata d=%g: model %.1f, Hata %.1f\n", d, q.pathLossDb, hataOpen(450, 50, 1.5, d));
        NEAR(q.pathLossDb, hataOpen(450, 50, 1.5, d), 6.0);
    }
    for (double d : {5.0, 15.0}) NEAR(evaluate(vhf(900, 30, 1.5), d, 0).pathLossDb, hataOpen(900, 30, 1.5, d), 8.0);   // plane earth has no f term
}

static void diffraction() {
    // knife edge, P.526: J(0) = 6.0 dB at grazing, nothing below v = -0.78, 13.9 dB at v = 1
    NEAR(knifeEdgeJ(0), 6.03, 0.05);
    NEAR(knifeEdgeJ(-0.78), 0, 1e-9);
    NEAR(knifeEdgeJ(-2), 0, 1e-9);
    NEAR(knifeEdgeJ(1), 13.9, 0.1);
    NEAR(knifeEdgeJ(2.4), 20.5, 0.2);
    CHECK(knifeEdgeJ(3) > knifeEdgeJ(2));
    NEAR(fresnelV(10, 5, 5, 299.792458), 10 * std::sqrt(2.0 * (2.0 / 5000.0)), 1e-6);
    // smooth earth (P.526 3.2): no loss with clearance, no step at the horizon (the old model jumped ~47 dB there), growing beyond
    CHECK(diffractionLossDb(150, 0.5, 30, 10) == 0);   // clearance above 0.552 of the first Fresnel zone
    CHECK(diffractionLossDb(150, 2, 30, 10) < 3);      // a little less clearance: a small loss
    const double h = radioHorizonKm(30, 2);
    NEAR(diffractionLossDb(150, h - 0.05, 30, 2), diffractionLossDb(150, h + 0.05, 30, 2), 1.0);
    for (double d = 1; d < 3 * h; d += 0.5) CHECK(diffractionLossDb(150, d + 0.5, 30, 2) >= diffractionLossDb(150, d, 30, 2) - 1e-9);
    const double a = diffractionLossDb(150, h + 20, 30, 2), b = diffractionLossDb(150, h + 60, 30, 2), c = diffractionLossDb(150, h + 120, 30, 2);
    CHECK(a > 0 && b > a && c > b);
    CHECK(diffractionLossDb(450, h + 60, 30, 2) > b);    // UHF is hurt more than VHF
    CHECK(diffractionLossDb(150, h + 60, 100, 2) < b);   // a taller mast reaches farther
    // the whole path loss is continuous across the horizon and monotonic in distance (Auto model)
    Params p = vhf(145.5, 10, 5);
    double prev = 0;
    for (double d = 0.5; d < 200; d *= 1.05) {
        const double L = evaluate(p, d, 0).pathLossDb;
        CHECK(L >= prev - 1.5);   // the plane-earth envelope has no ripple; allow the blend a little slack
        if (d > 1) CHECK(L - prev < 6);
        prev = L;
    }
    // troposcatter (P.452): roughly 50..80 dB beyond free space at 200 km, growing with distance
    const double ts = troposcatterLossDb(2000, 200, 150, 10) - fsplDb(2000, 200);
    INRANGE(ts, 45, 80);
    CHECK(troposcatterLossDb(100, 300, 150, 10) > troposcatterLossDb(100, 200, 150, 10));
}

static void broadcastBand() {
    // P.1546-style sanity band: 100 MHz, 150 m mast, 10 m receiver, 1 kW ERP, land. The published curves are about 85 / 57 / 40 / 20 dBuV/m at
    // 10 / 50 / 100 / 200 km; this simple model must stay within a broad band of them (it has no time-percentage statistics).
    Params b = vhf(100, 150, 10);
    b.txPowerDbw = 30; b.txGainDbi = 2.15;
    INRANGE(evaluate(b, 10, 0).fieldDbuvm, 75, 92);
    INRANGE(evaluate(b, 50, 0).fieldDbuvm, 42, 65);
    INRANGE(evaluate(b, 100, 0).fieldDbuvm, 20, 48);
    INRANGE(evaluate(b, 200, 0).fieldDbuvm, 0, 30);
    // ADS-B: an aircraft at 10 km is heard to about its radio horizon
    Params ads = vhf(1090, 10000, 10);
    ads.txPowerDbw = 24; ads.bandwidthHz = 2e6; ads.requiredSnrDb = 10; ads.rxGainDbi = 3; ads.rxNfDb = 4;
    const Range r = findRange(ads);
    INRANGE(r.maxKm, 300, 600);
}

static void clutterRainGas() {
    CHECK(clutterLossDb(600, 2, Clutter::Open) == 0);
    CHECK(clutterLossDb(600, 25, Clutter::Urban) == 0);          // above the buildings
    INRANGE(clutterLossDb(600, 2, Clutter::Urban), 15, 25);
    INRANGE(clutterLossDb(100, 2, Clutter::Suburban), 5, 14);
    CHECK(clutterLossDb(2000, 2, Clutter::DenseUrban) > clutterLossDb(2000, 2, Clutter::Suburban));
    CHECK(clutterLossDb(10, 2, Clutter::Urban) == 0);            // not below 30 MHz
    // rain: P.838 at 10 GHz, 25 mm/h about 0.7 dB/km; 20 GHz about 2.7 dB/km; nothing at UHF
    INRANGE(rainSpecificDbKm(10000, 25), 0.6, 0.8);
    INRANGE(rainSpecificDbKm(20000, 25), 2.3, 3.1);
    CHECK(rainSpecificDbKm(433, 100) == 0);
    CHECK(rainLossDb(10000, 50, 25) < rainSpecificDbKm(10000, 25) * 50);   // rain cells are smaller than the path
    // gases: the 22 GHz water and 60 GHz oxygen lines
    CHECK(gasSpecificDbKm(22200) > gasSpecificDbKm(10000) * 5);
    INRANGE(gasSpecificDbKm(60000), 10, 20);
    CHECK(gasSpecificDbKm(500) < 0.01);   // P.676: about 0.006 dB/km at 500 MHz (it adds up only over long paths)
    // polarisation
    CHECK(polLossDb(Pol::Vertical, Pol::Vertical) == 0);
    NEAR(polLossDb(Pol::Vertical, Pol::Horizontal), 20, 1e-9);
    NEAR(polLossDb(Pol::Vertical, Pol::CircularRight), 3, 1e-9);
    NEAR(polLossDb(Pol::CircularLeft, Pol::CircularRight), 20, 1e-9);
    Params p = vhf(145, 10, 5);
    Params q = p; q.rxPol = Pol::Horizontal;
    NEAR(evaluate(p, 10, 0).rxDbm - evaluate(q, 10, 0).rxDbm, 20, 1e-6);
    Params c = p; c.rxClutter = Clutter::Urban;
    CHECK(evaluate(c, 10, 0).rxDbm < evaluate(p, 10, 0).rxDbm - 5);
}

static void groundWave() {
    // P.368 behaviour: over sea at 1 MHz the ground wave is nearly free space at 100 km; average land loses 15..30 dB there
    INRANGE(groundWaveLossDb(1, 100, Ground::Sea, Pol::Vertical) - fsplDb(1, 100), -0.5, 2);
    INRANGE(groundWaveLossDb(1, 100, Ground::Average, Pol::Vertical) - fsplDb(1, 100), 15, 32);
    CHECK(groundWaveFactor(7.1, 100, Ground::Sea) > groundWaveFactor(7.1, 100, Ground::Dry));
    NEAR(groundWaveFactor(7.1, 0.0, Ground::Average), 1.0, 1e-9);
    CHECK(groundWaveFactor(7.1, 200, Ground::Average) < groundWaveFactor(7.1, 20, Ground::Average));
    // higher frequencies die faster over land; horizontal polarisation dies faster than vertical
    CHECK(groundWaveLossDb(7, 50, Ground::Average, Pol::Vertical) - fsplDb(7, 50) > groundWaveLossDb(1, 50, Ground::Average, Pol::Vertical) - fsplDb(1, 50) + 10);
    CHECK(groundWaveLossDb(7, 50, Ground::Average, Pol::Horizontal) > groundWaveLossDb(7, 50, Ground::Average, Pol::Vertical));
    // the earth's curvature: far out the loss grows faster than the flat-earth Norton value
    const double flat300 = fsplDb(3.5, 300) - 20 * std::log10(groundWaveFactor(3.5, 300, Ground::Average));
    CHECK(groundWaveLossDb(3.5, 300, Ground::Average, Pol::Vertical) >= flat300);
    CHECK(groundWaveLossDb(3.5, 1000, Ground::Average, Pol::Vertical) > fsplDb(3.5, 1000) - 20 * std::log10(groundWaveFactor(3.5, 1000, Ground::Average)) + 10);
}

static Params hfPath(double f, double hourUtc, int month, double sfi) {
    Params h; h.txLat = 54.7; h.txLon = 25.3; h.fMHz = f; h.txPowerDbw = 20; h.txGainDbi = 2.15; h.rxGainDbi = 2.15; h.env = Env::Rural;
    h.bandwidthHz = 2400; h.sfi = sfi; h.year = 2026; h.month = month; h.day = 21; h.utcHour = hourUtc; h.requiredSnrDb = 10;
    h.txPol = h.rxPol = Pol::Horizontal; h.txHeightM = h.rxHeightM = 10;
    h.hfLegacy = true;
    return h;
}

static void skywave() {
    // geometry: vertical at zero distance; the single-hop limit near 3800 km at zero elevation; M(3000)F2 ~ 3
    NEAR(hopIncidenceSec(0, 300), 1.0, 1e-9);
    INRANGE(hopIncidenceSec(3000, 300), 2.8, 3.6);
    NEAR(hopMaxKm(300, 0), 3840, 60);
    INRANGE(hopMaxKm(300, 3), 3000, 3400);
    NEAR(hopElevationDeg(hopMaxKm(300, 3), 300), 3, 0.01);
    NEAR(hopElevationDeg(0.001, 300), 90, 0.1);
    INRANGE(hopElevationDeg(1000, 300), 25, 31);
    // the ray reaches the D layer far less obliquely than a ray reflected at the D layer itself (the old model's 43 dB at 7.1 MHz)
    INRANGE(dLayerSec(1000, 300, 100), 1.8, 2.3);
    // sun and sunspots
    NEAR(sfiToSunspot(63.7), 0, 1e-9);
    NEAR(sfiToSunspot(63.7 + 0.728 * 100 + 0.00089 * 1e4), 100, 0.01);
    NEAR(solarCosZenith(0, 0, 2026, 3, 21, 12.0), 1.0, 0.02);
    CHECK(solarCosZenith(0, 0, 2026, 3, 21, 0.0) < -0.95);
    CHECK(solarCosZenith(60, 0, 2026, 6, 21, 12.0) > solarCosZenith(60, 0, 2026, 12, 21, 12.0));
    // foF2 at a mid-latitude in summer, SFI 120: daytime about 6..11 MHz, night 2.5..6, higher with more flux, lower in a storm
    const Params day = hfPath(7.1, 10.0, 6, 120), night = hfPath(7.1, 22.0, 6, 120);
    const double fo = foF2(54.7, 25.3, day);
    INRANGE(fo, 6, 11);
    INRANGE(foF2(54.7, 25.3, night), 2.5, 6);
    Params hiSfi = day; hiSfi.sfi = 200;
    CHECK(foF2(54.7, 25.3, hiSfi) > fo);
    Params storm = day; storm.kIndex = 7;
    CHECK(foF2(54.7, 25.3, storm) < fo * 0.9);
    // MUF over 1000 km: about 9..16 MHz at noon, 4..9 at night; the oblique MUF is above foF2
    INRANGE(mufMHz(54.7, 30, 1000, day), 9, 16);
    INRANGE(mufMHz(54.7, 30, 1000, night), 4, 9);
    CHECK(mufMHz(54.7, 25.3, 2000, day) > fo && mufMHz(54.7, 25.3, 3000, day) > mufMHz(54.7, 25.3, 1000, day));
    // D-layer absorption at 7.1 MHz over 1000 km at noon in summer: 10..30 dB; at night a few dB; much less at 14 MHz
    const Point d7 = evaluate(day, 1000, 90), n7 = evaluate(night, 1000, 90);
    INRANGE(d7.absorptionDb, 10, 30);
    CHECK(n7.absorptionDb < 4);
    Params d14 = day; d14.fMHz = 14.2;
    CHECK(evaluate(d14, 1000, 90).absorptionDb < d7.absorptionDb / 2.5);
    CHECK(d7.hops == 1 && evaluate(day, 6000, 90).hops == 2);
    CHECK(evaluate(day, 1500, 90).model == Model::HfSky);
    CHECK(evaluate(day, 3, 90).model == Model::HfGround);
    // above-MUF scatter: a little above the MUF loses a few dB, far above it there is no skywave
    NEAR(aboveMufLossDb(10, 10), 0, 1e-12);
    NEAR(aboveMufLossDb(11, 10), 1.3, 0.01);
    Params t = night; t.fMHz = 28.0;
    CHECK(evaluate(t, 1500, 90).hops == 0);
    // skip zone: 20 m by day, ground wave close in, nothing for a few hundred km, then the skywave
    const Range r20 = findRange(d14, 90);
    CHECK(r20.spans.size() >= 2);
    if (r20.spans.size() >= 2) { INRANGE(r20.spans[0].second, 1, 80); INRANGE(r20.spans[1].first, 300, 2000); CHECK(r20.maxKm > 1500); }
    // band closed: 10 m at night near solar minimum reaches only as far as its ground wave
    const Range r10 = findRange(hfPath(28.5, 23.0, 12, 70), 90);
    CHECK(r10.maxKm < 100);
    // 80 m at night: hundreds of km and more
    CHECK(findRange(hfPath(3.65, 22.0, 6, 120), 90).maxKm > 500);
    // the HF ranges no longer collapse: 40 m by day with 100 W reaches several hundred km (NVIS and short skip)
    INRANGE(findRange(day, 90).maxKm, 300, 2500);
    // HF ground / sky selection
    Params g = day; g.hfModel = HfModel::GroundOnly;
    CHECK(evaluate(g, 1500, 90).hops == 0 && evaluate(g, 1500, 90).model == Model::HfGround);
    Params s = day; s.hfModel = HfModel::SkyOnly;
    CHECK(evaluate(s, 3, 90).model == Model::HfSky);
}

static void noise() {
    NEAR(ktbDbm(1), -174.0, 1e-9);
    NEAR(ktbDbm(1e6), -114.0, 1e-9);
    NEAR(ktbDbm(12500), -174 + 40.97, 0.01);
    // P.372 man-made: Fa = c - d log f
    NEAR(manMadeFa(100, Env::City), 76.8 - 27.7 * 2, 1e-9);
    NEAR(manMadeFa(10, Env::Residential), 72.5 - 27.7, 1e-9);
    NEAR(manMadeFa(1, Env::Rural), 67.2, 1e-9);
    NEAR(manMadeFa(10, Env::Quiet), 53.6 - 28.6, 1e-9);
    NEAR(galacticFa(30), 52 - 23 * std::log10(30.0), 1e-9);
    CHECK(galacticFa(3) < -50);                     // the ionosphere keeps galactic noise out at low HF
    CHECK(externalNoiseFa(1000, Env::Quiet) < 0.5); // negligible at 1 GHz (floored at 0 dB = 290 K)
    CHECK(externalNoiseFa(7, Env::City) > externalNoiseFa(7, Env::Rural));
    // atmospheric noise: a night-time HF effect, strongest on the low bands, gone at VHF
    const double an = atmosphericFa(3.5, 54.7, 25.3, 2026, 7, 1, 22.0), ad = atmosphericFa(3.5, 54.7, 25.3, 2026, 7, 1, 10.0);
    CHECK(an > ad + 15);
    CHECK(an > manMadeFa(3.5, Env::Rural));         // summer nights on 80 m are lightning-limited in the country
    CHECK(atmosphericFa(1.0, 54.7, 25.3, 2026, 7, 1, 22.0) > an);
    CHECK(atmosphericFa(50, 54.7, 25.3, 2026, 7, 1, 22.0) < -50);
    CHECK(atmosphericFa(3.5, 10, 25.3, 2026, 7, 1, 22.0) > an);   // the tropics are noisier
    // receive chain (Friis) and the noise floor
    NEAR(cascadeNfDb(0, 6, false, 0, 0), 6, 1e-9);
    NEAR(cascadeNfDb(3, 6, false, 0, 0), 9, 1e-9);
    NEAR(cascadeNfDb(3, 6, true, 20, 1), 10 * std::log10(std::pow(10, 0.1) + (std::pow(10, 0.9) - 1) / 100), 1e-9);
    CHECK(cascadeNfDb(3, 6, true, 20, 1) < 1.5);
    NEAR(systemNoiseDbm(1500, 1e6, 6, Env::Quiet), -108, 0.05);
    NEAR(systemNoiseDbm(12500, 0, 6), -174 + 40.97 + 6, 0.01);
    CHECK(systemNoiseDbm(7, 2400, 6, Env::City) > systemNoiseDbm(7, 2400, 6, Env::Quiet) + 20);
}

static void linkBudget() {
    Params v = vhf(145.5, 10, 5);
    v.txPowerDbw = 14; v.txGainDbi = 5; v.txFeederDb = 2; v.rxGainDbi = 3; v.rxFeederDb = 1.5; v.rxNfDb = 5;
    const Point a = evaluate(v, 20, 0);
    NEAR(eirpDbw(v), 17, 1e-9);
    NEAR(a.eirpDbw, 17, 1e-9);
    NEAR(a.rxDbm, 14 + 30 - 2 + 5 - a.pathLossDb + 3, 1e-9);
    NEAR(a.pathLossDb, a.fsDb + a.excessDb + a.clutterDb + a.rainDb + a.gasDb + a.polDb, 1e-9);
    NEAR(a.sysNfDb, 6.5, 1e-9);
    NEAR(a.faDb, 10 * std::log10(std::pow(10, galacticFa(145.5) / 10) + std::pow(10, manMadeFa(145.5, Env::Quiet) / 10)), 1e-9);   // galactic ~2 dB
    NEAR(a.noiseDbm, -174 + 10 * std::log10(12500.0) + 10 * std::log10(std::pow(10, a.faDb / 10) + std::pow(10, 0.65) - 1), 0.01);
    NEAR(a.snrDb, a.rxDbm - a.noiseDbm, 1e-9);
    NEAR(a.marginDb, a.snrDb - 12, 1e-9);
    NEAR(a.fieldDbuvm, a.rxDbm - 3 + 20 * std::log10(145.5) + 77.2, 1e-9);
    // range: margin >= 0 up to maxKm, below zero just beyond it, monotonic in power and in the SNR needed
    const Range r = findRange(v);
    CHECK(r.any && r.maxKm > 10);
    CHECK(evaluate(v, r.maxKm * 1.1, 0).marginDb < 0);
    Params v2 = v; v2.txPowerDbw += 20;
    CHECK(findRange(v2).maxKm > r.maxKm);
    Params v3 = v; v3.requiredSnrDb += 20;
    CHECK(findRange(v3).maxKm < r.maxKm);
    Params v4 = v; v4.preamp = true;
    CHECK(evaluate(v4, 20, 0).snrDb > a.snrDb);
    // grid: the transmitter cell is the strongest; same values as evaluateAt; cancel stops it
    Grid g = computeGrid(v, 50, 21);
    CHECK(g.n == 21 && g.snr.size() == 441);
    CHECK(g.snr[10 * 21 + 10] > g.snr[0]);
    const double la = g.lat0 + (g.lat1 - g.lat0) * 10.5 / 21, lo = g.lon0 + (g.lon1 - g.lon0) * 10.5 / 21;
    NEAR(g.snr[10 * 21 + 10], evaluateAt(v, la, lo).snrDb, 1e-3);
    std::atomic<bool> stop{true};
    CHECK(computeGrid(v, 50, 21, &stop).snr.empty());
    // geodesy
    NEAR(greatCircleKm(0, 0, 0, 1), 111.19, 0.05);
    double la2, lo2; destination(54.7, 25.3, 90, 500, la2, lo2);
    NEAR(greatCircleKm(54.7, 25.3, la2, lo2), 500, 0.01);
    NEAR(bearingDeg(0, 0, 0, 1), 90, 1e-6);
    NEAR(bearingDeg(0, 0, 1, 0), 0, 1e-6);
    double ml, mo; midpoint(0, 0, 0, 90, ml, mo);
    NEAR(ml, 0, 1e-9); NEAR(mo, 45, 1e-9);
    CHECK(std::string(modelName(hfPath(7, 12, 6, 120))) != std::string(modelName(v)));
    CHECK(!modelLimits(hfPath(7, 12, 6, 120)).empty() && !modelLimits(v).empty());
}

static void antennaPattern() {
    AntennaPattern ap;
    std::string err;
    CHECK(parseAntennaPattern("# yagi\nazimuth,gain\n0,10\n90,-5\n180,-10\n270,-5\n", ap, err));
    CHECK(ap.valid());
    NEAR(ap.gainDbi(0, 0), 10, 1e-9);
    NEAR(ap.gainDbi(45, 0), 2.5, 1e-9);
    NEAR(ap.gainDbi(-90, 0), -5, 1e-9);
    NEAR(ap.gainDbi(315, 0), 2.5, 1e-9);
    CHECK(!parseAntennaPattern("0,1\n", ap, err) && !err.empty());
    CHECK(!parseAntennaPattern("0,1\n90,x\n", ap, err));
    AntennaPattern el;
    CHECK(parseAntennaPattern("0,6\n180,6\nel,-90,-30\nel,0,0\nel,30,-3\nel,90,-30\n", el, err));
    NEAR(el.gainDbi(0, 0), 6, 1e-9);
    NEAR(el.gainDbi(0, 30), 3, 1e-9);
    // the pattern replaces the scalar gain, by bearing from the boresight
    Params v = vhf(145.5, 10, 5);
    v.txPattern = &ap; v.txAzDeg = 90;
    NEAR(evaluate(v, 10, 90).txGainDbi, 10, 1e-9);
    NEAR(evaluate(v, 10, 270).txGainDbi, -10, 1e-9);
}

// 4.0 defaults: P.1812-8 over a flat profile (no terrain loaded) and P.533 for the HF skywave, through evaluate()
static void itu40() {
    Params b = vhf(100, 150, 10);
    b.pathModel = PathModel::Auto; b.txPowerDbw = 30; b.txGainDbi = 2.15;
    // P.1546 land curves for 100 MHz, 150 m, 50 % time / locations: about 85 / 57 / 40 / 20 dBuV/m at 10 / 50 / 100 / 200 km
    const Point p10 = evaluate(b, 10, 0), p50 = evaluate(b, 50, 0), p100 = evaluate(b, 100, 0), p200 = evaluate(b, 200, 0);
    // (on a flat profile P.1812 is pure spherical-earth diffraction beyond the horizon, so it sits below the empirical P.1546 curves far out;
    //  printed so the comparison is in the log)
    printf("  P.1812 flat, 100 MHz 150 m: %.1f / %.1f / %.1f / %.1f dBuV/m at 10 / 50 / 100 / 200 km (P.1546: about 85 / 57 / 40 / 20)\n",
           p10.fieldDbuvm, p50.fieldDbuvm, p100.fieldDbuvm, p200.fieldDbuvm);
    INRANGE(p10.fieldDbuvm, 75, 95); INRANGE(p50.fieldDbuvm, 45, 68); INRANGE(p100.fieldDbuvm, 20, 50); INRANGE(p200.fieldDbuvm, 0, 32);
    CHECK(p50.flatEarth && !p50.terrainUsed);
    INRANGE(p50.kFactor, 1.3, 1.45);
    // 1 % of the time the far signal is stronger (ducting / super-refraction); 90 % of locations costs about 1.28 x 5.5 dB
    Params t1 = b; t1.timePct = 1;
    CHECK(evaluate(t1, 200, 0).fieldDbuvm > p200.fieldDbuvm + 3);
    Params l90 = b; l90.locPct = 90;
    NEAR(p50.fieldDbuvm - evaluate(l90, 50, 0).fieldDbuvm, 1.2816 * 5.5, 0.3);
    // ADS-B from an aircraft at 10 km: heard to about the radio horizon
    Params ads = vhf(1090, 10000, 10);
    ads.pathModel = PathModel::Auto; ads.txPowerDbw = 24; ads.bandwidthHz = 2e6; ads.requiredSnrDb = 10; ads.rxGainDbi = 3; ads.rxNfDb = 4;
    INRANGE(findRange(ads).maxKm, 300, 650);
    // HF: 20 m over 1500 km on a spring day uses the P.533 skywave and reports a reliability
    Params h = hfPath(14.1, 11.0, 4, 150);
    h.hfLegacy = false;
    const Point q = evaluate(h, 1500, 270);
    CHECK(q.model == Model::HfSky);
    CHECK(q.hfReliability >= 0 && q.hfReliability <= 1);
    CHECK(q.muf > 14.1 && q.hops >= 1);
    CHECK(q.snrDb > 10);
    // cost of the map (worker thread): an 80 x 80 grid with P.1812 and with P.533
    {
        const auto t0 = std::chrono::steady_clock::now();
        const Grid gv = computeGrid(b, 200, 80);
        const auto t1 = std::chrono::steady_clock::now();
        const Grid gh = computeGrid(h, 3000, 80);
        const auto t2 = std::chrono::steady_clock::now();
        const double sv = std::chrono::duration<double>(t1 - t0).count(), sh = std::chrono::duration<double>(t2 - t1).count();
        printf("  80 x 80 map: P.1812 flat %.2f s, HF P.533 %.2f s\n", sv, sh);
        CHECK(gv.snr.size() == 6400 && gh.snr.size() == 6400);
        CHECK(sv < 20 && sh < 30);
    }
}

int main() {
    freeSpace();
    twoRayAndHorizon();
    diffraction();
    broadcastBand();
    clutterRainGas();
    groundWave();
    skywave();
    noise();
    linkBudget();
    antennaPattern();
    itu40();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("prop_model: ok\n");
    return 0;
}
