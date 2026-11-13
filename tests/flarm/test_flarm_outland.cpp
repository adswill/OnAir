// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Out-landing alert (flarm_outland.h): simulated glider flights with GNSS noise, speed jitter and lost packets.
#include "dect2/flarm/flarm_outland.h"
#include <cmath>
#include <random>
#include "../common/test_util.h"
using namespace dect2;

// a flight of flySec at 25 m/s heading east from (54.9, 23.9), a 40 s roll-out, then standing still for standSec. Returns the alerts.
static int fly(OutlandingWatch& w, uint32_t id, double flySec, double standSec, double loss, double posNoiseM, unsigned seed, OutlandingAlert* last, double t0 = 0) {
    std::mt19937 r(seed); std::normal_distribution<double> g(0, 1); std::uniform_real_distribution<double> u(0, 1);
    int alerts = 0; double x = 0;
    for (double t = 0; t < flySec + 40 + standSec; t += 1) {
        double v = t < flySec ? 25 : t < flySec + 40 ? 25 * (1 - (t - flySec) / 40) : 0;
        x += v;
        if (u(r) < loss) continue;
        TrafficFix f; f.id = id; f.t = t0 + t;
        const double nx = x + posNoiseM * g(r), ny = posNoiseM * g(r);
        f.lat = 54.9 + ny / 111320.0; f.lon = 23.9 + nx / (111320.0 * std::cos(54.9 * M_PI / 180));
        f.speedMs = std::max(0.0, v + 0.8 * g(r)); f.climbMs = t < flySec ? 1.5 * g(r) : 0.2 * g(r);
        OutlandingAlert a; if (w.add(f, &a)) { alerts++; if (last) *last = a; }
    }
    return alerts;
}

int main() {
    // the airfield table: one airfield at the start (54.9, 23.9) and one 20 km east (the far end of a 13 min flight at 25 m/s)
    auto fields = [](double lat, double lon) {
        const double a = OutlandingWatch::distM(lat, lon, 54.9, 23.9) / 1000, b = OutlandingWatch::distM(lat, lon, 54.9, 23.9 + 20000 / (111320.0 * std::cos(54.9 * M_PI / 180))) / 1000;
        const double m = std::min(a, b); return m < 50 ? m : -1;
    };
    for (double loss : {0.0, 0.3, 0.6}) for (double noise : {3.0, 15.0}) {
        OutlandingWatch w; w.nearestAirfieldKm = fields;
        OutlandingAlert a;
        // 30 min flight: lands 45 km east, 25 km past the second field
        CHECK(fly(w, 1, 1800, 600, loss, noise, 11, &a) == 1, "loss %.1f noise %.0f: one out-landing alert", loss, noise);
        CHECK(a.id == 1 && a.flownSec > 1500 && a.airfieldKm > 20, "alert data %.0f %.1f", a.flownSec, a.airfieldKm);
        // 13 min flight: lands 0.3 km past the second airfield: a normal landing
        CHECK(fly(w, 2, 800 - 8, 600, loss, noise, 12, nullptr) == 0, "loss %.1f noise %.0f: landing at an airfield", loss, noise);
        // 5 min flight ending 7.5 km from the start and 12.5 km from the other field: out-landing with the distance
        CHECK(fly(w, 3, 300, 600, loss, noise, 13, &a) == 1 && a.airfieldKm > 6 && a.airfieldKm < 9, "loss %.1f: out-landing %.1f km from a field", loss, a.airfieldKm);
        // a 60 s hop is not a flight
        CHECK(fly(w, 4, 60, 600, loss, noise, 14, nullptr) == 0, "short hop");
        // standing for 15 s only (signal lost on the ground) is not decided yet
        CHECK(fly(w, 5, 1800, 15, loss, noise, 15, nullptr) == 0, "too short standing");
    }
    {   // the same aircraft lands out twice in one day: two alerts, each once
        OutlandingWatch w; w.nearestAirfieldKm = [](double, double) { return -1.0; };
        int n = fly(w, 9, 600, 300, 0, 5, 1, nullptr, 0);
        n += fly(w, 9, 600, 300, 0, 5, 2, nullptr, 5000);
        CHECK(n == 2, "two landings %d", n);
    }
    {   // an aircraft that says it is on the ground while fast (a car-towed launch, a taxiing tug) is not flying
        OutlandingWatch w;
        for (int t = 0; t < 400; t++) { TrafficFix f; f.id = 7; f.t = t; f.lat = 54; f.lon = 23 + t * 1e-5 * (t < 200); f.speedMs = t < 200 ? 20 : 0; f.reportsGround = true; CHECK(!w.add(f), "ground"); }
    }
    return testSummary("flarm_outland");
}
