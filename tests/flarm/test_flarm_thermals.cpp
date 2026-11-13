// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Thermals map (flarm_thermals.h): simulated gliders circling in drifting thermals, with GNSS noise, lost packets and climb noise;
// straight glides and sinking circles must not count. Simulated only: no real FLARM recording of a thermal.
#include "dect2/flarm/flarm_thermals.h"
#include <cmath>
#include <random>
#include "../common/test_util.h"
using namespace dect2;

constexpr double kLat = 54.9, kLon = 23.9;
static double mx(double lon) { return (lon - kLon) * 111320.0 * std::cos(kLat * M_PI / 180); }
static double my(double lat) { return (lat - kLat) * 111320.0; }

// a glider: `straight` s of straight flight east, then circling for `circleSec` around (cx, cy) m, radius r, period p, climb, wind drift (wx, wy)
static int glider(ThermalMap& m, uint32_t id, double t0, double straight, double circleSec, double cx, double cy, double radius, double period, double climb,
                  double wx, double wy, double noiseM, double loss, unsigned seed, Thermal* fresh) {
    std::mt19937 r(seed); std::normal_distribution<double> g(0, 1); std::uniform_real_distribution<double> u(0, 1);
    int created = 0; double alt = 1200;
    for (double t = 0; t < straight + circleSec; t += 1) {
        double x, y, v = 2 * M_PI * radius / period;
        if (t < straight) { x = cx - radius - 25 * (straight - t); y = cy; v = 25; alt -= 1; }
        else { const double a = 2 * M_PI * (t - straight) / period; x = cx + wx * (t - straight) - radius * std::cos(a); y = cy + wy * (t - straight) + radius * std::sin(a); alt += climb; }
        if (u(r) < loss) continue;
        TrafficFix f; f.id = id; f.t = t0 + t;
        f.lat = kLat + (y + noiseM * g(r)) / 111320.0; f.lon = kLon + (x + noiseM * g(r)) / (111320.0 * std::cos(kLat * M_PI / 180));
        f.altM = std::round(alt + 2 * g(r)); f.speedMs = v + g(r); f.climbMs = (t < straight ? -1 : climb) + 0.5 * g(r);
        Thermal th;
        if (m.add(f, &th)) { created++; if (fresh) *fresh = th; }
    }
    return created;
}

int main() {
    for (unsigned seed = 1; seed <= 4; seed++) {
        const double loss = 0.1 * (seed - 1), noise = 3 + 2 * seed;
        ThermalMap m;
        Thermal th;
        // a 2.2 m/s thermal drifting 4 m/s north-east; the glider circles 5 minutes
        int c = glider(m, 0x111111, 0, 60, 300, 0, 0, 90, 26, 2.2, 3, 3, noise, loss, seed, &th);
        CHECK(c == 1, "seed %u: %d thermals created", seed, c);
        auto act = m.active(400);
        CHECK(act.size() == 1, "seed %u: %zu active", seed, act.size());
        if (!act.empty()) {
            const Thermal& a = act[0];
            const double ex = 3 * 300.0, ey = 3 * 300.0;   // the centre has drifted with the wind
            const double d = std::hypot(mx(a.lon) - ex, my(a.lat) - ey);
            CHECK(d < 150, "seed %u: centre %.0f m off", seed, d);
            CHECK(std::fabs(a.climbMs - 2.2) < 0.4, "seed %u: climb %.2f", seed, a.climbMs);
            CHECK(a.topM - a.baseM > 400, "seed %u: band %.0f - %.0f", seed, a.baseM, a.topM);
        }
        // a second glider joins the same thermal a few minutes later: no new thermal, two aircraft
        c = glider(m, 0x222222, 300, 30, 200, 3 * 330, 3 * 330, 70, 24, 1.8, 3, 3, noise, loss, seed + 10, nullptr);
        act = m.active(560);
        CHECK(c == 0 && !act.empty() && act[0].aircraft.size() == 2, "seed %u: join made %d new, %zu aircraft", seed, c, act.empty() ? 0 : act[0].aircraft.size());
        // a glider circling in sink (-1 m/s) and one gliding straight: nothing
        c = glider(m, 0x333333, 600, 60, 200, 5000, 0, 80, 25, -1.0, 0, 0, noise, loss, seed + 20, nullptr);
        CHECK(c == 0, "seed %u: sinking circle made %d", seed, c);
        c = glider(m, 0x444444, 600, 600, 0, 9000, 2000, 80, 25, 0, 0, 0, noise, loss, seed + 30, nullptr);
        CHECK(c == 0, "seed %u: straight glide made %d", seed, c);
        // another thermal 4 km away: a new one
        c = glider(m, 0x555555, 900, 30, 120, -4000, 0, 100, 28, 1.2, 0, 0, noise, loss, seed + 40, nullptr);
        CHECK(c == 1 && m.active(1100).size() == 2, "seed %u: second thermal made %d (%zu active)", seed, c, m.active(1100).size());
        m.prune(1100 + 4 * 3600);
        CHECK(m.thermals().empty(), "seed %u: prune left %zu", seed, m.thermals().size());
    }
    return testSummary("flarm_thermals");
}
