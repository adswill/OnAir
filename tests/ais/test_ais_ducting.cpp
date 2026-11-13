// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AIS ducting detector (ais_ducting.h): simulated traffic around a coastal receiver for two days, with a two-hour duct that brings ships
// from 120 - 260 km; SAR aircraft, base stations, wrong positions and the satellite channels must not start an event; one far ship alone
// must not either. Simulated only: no real recording of a ducting night was used.
#include "dect2/ais/ais_ducting.h"
#include <cmath>
#include <random>
#include "../common/test_util.h"
using namespace dect2;

constexpr double kLat = 55.7, kLon = 21.1;
static void at(double km, double brg, double& lat, double& lon) {
    const double d = km / 6371.0, b = brg * M_PI / 180, p1 = kLat * M_PI / 180, l1 = kLon * M_PI / 180;
    const double p2 = std::asin(std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(b));
    lat = p2 * 180 / M_PI; lon = (l1 + std::atan2(std::sin(b) * std::sin(d) * std::cos(p1), std::cos(d) - std::sin(p1) * std::sin(p2))) * 180 / M_PI;
}

int main() {
    for (unsigned seed = 1; seed <= 3; seed++) {
        std::mt19937 r(seed); std::uniform_real_distribution<double> u(0, 1);
        AisDucting d;
        d.setHome(kLat, kLon);
        std::vector<AisDuctEvent> ev;
        const double t0 = 1767225600;
        struct Ship { uint32_t mmsi; double km, brg; };
        std::vector<Ship> nearV;
        for (int i = 0; i < 60; i++) nearV.push_back({(uint32_t)(277000000 + i), 5 + 35 * u(r), 180 + 180 * u(r)});   // west of the coast
        const double ductFrom = t0 + 30 * 3600, ductTo = ductFrom + 2 * 3600;
        int eventsBefore = 0;
        for (double t = t0; t < t0 + 48 * 3600; t += 60) {
            for (auto& s : nearV) {
                s.km = std::clamp(s.km + (u(r) - 0.5) * 0.4, 3.0, 40.0);
                double la, lo; at(s.km, s.brg, la, lo);
                d.add(t + u(r) * 50, s.mmsi, la, lo, u(r) < 0.7 ? "A" : "B", 161.975e6, ev);
            }
            // never a duct: a SAR aircraft and a base station far away, a broken position, a long-range channel report, one far ship now and then
            double la, lo;
            at(220, 250, la, lo); d.add(t + 51, 111277500, la, lo, "SAR", 161.975e6, ev);
            at(150, 230, la, lo); d.add(t + 52, 2770001, la, lo, "base", 162.025e6, ev);
            d.add(t + 53, 277999001, 0, 0, "A", 161.975e6, ev);
            at(5000, 270, la, lo); d.add(t + 54, 277999002, la, lo, "A", 161.975e6, ev);
            at(300, 260, la, lo); d.add(t + 55, 277999003, la, lo, "A", 156.775e6, ev);
            if ((int)((t - t0) / 60) % 97 == 0) { at(130, 240, la, lo); d.add(t + 56, 277999004, la, lo, "A", 161.975e6, ev); }
            if (t >= ductFrom && t < ductTo)
                for (int i = 0; i < 8; i++) { at(120 + 17 * i + 5 * u(r), 255 + 10 * u(r), la, lo); d.add(t + 57, 265000000 + i, la, lo, "A", 162.025e6, ev); }
            if (t < ductFrom) eventsBefore = (int)ev.size();
            if ((int)(t - t0) % 600 == 0) d.tick(t, ev);
        }
        CHECK(eventsBefore == 0, "seed %u: %d events before the duct (%s)", seed, eventsBefore, ev.empty() ? "" : ev[0].text.c_str());
        CHECK(ev.size() == 2 && ev[0].start && !ev[1].start, "seed %u: %zu events", seed, ev.size());
        if (ev.size() == 2) {
            CHECK(ev[0].t >= ductFrom && ev[0].t < ductFrom + 600, "seed %u: started %.0f s into the duct", seed, ev[0].t - ductFrom);
            CHECK(ev[0].vessels >= 2 && ev[0].farthestKm > 120, "seed %u: %d vessels, %.0f km", seed, ev[0].vessels, ev[0].farthestKm);
            CHECK(ev[0].bearingDeg > 240 && ev[0].bearingDeg < 275, "seed %u: bearing %.0f", seed, ev[0].bearingDeg);
            CHECK(ev[1].t >= ductTo + 1700 && ev[1].t < ductTo + 3600, "seed %u: ended %.0f s after", seed, ev[1].t - ductTo);
        }
        CHECK(d.normalKm() > 33 && d.normalKm() < 42, "seed %u: normal range %.1f km", seed, d.normalKm());
        CHECK(!d.active(), "seed %u: still active", seed);
    }
    // before anything is learned the radio horizon is the normal range: 4.12 (sqrt 10 + sqrt 25) = 33.6 km
    AisDucting d;
    CHECK(std::fabs(d.normalKm() - 33.63) < 0.1, "horizon %.2f", d.normalKm());
    return testSummary("ais_ducting");
}
