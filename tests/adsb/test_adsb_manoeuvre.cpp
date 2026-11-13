// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ADS-B manoeuvre alerts (adsb_manoeuvre.h): simulated flights sent as 2 s updates with lost updates, rate and heading noise, and
// aircraft that send no vertical rate (the rate then comes from the altitude). Simulated only: no real recording of such a flight.
#include "dect2/adsb/adsb_manoeuvre.h"
#include <cmath>
#include <functional>
#include <random>
#include <string>
#include "../common/test_util.h"
using namespace dect2;

struct Seg { double sec, vrate, turnRate, speed; bool ground; };

// fly segments from altitude alt0; returns the alerts
static std::vector<AdsbManAlert> fly(const std::vector<Seg>& segs, double alt0, unsigned seed, double loss, bool sendVrate = true, int squawk = 1000, double t0 = 1000,
                                     const char* hex = "4840D6") {
    AdsbManoeuvreWatch w;
    std::mt19937 r(seed); std::normal_distribution<double> g(0, 1); std::uniform_real_distribution<double> u(0, 1);
    std::vector<AdsbManAlert> out;
    double t = t0, alt = alt0, hdg = 90;
    for (const auto& s : segs) {
        for (double e = 0; e < s.sec; e += 2) {
            t += 2; alt = std::max(0.0, alt + s.vrate / 30.0); hdg = std::fmod(hdg + s.turnRate * 2 + 360, 360);
            if (u(r) < loss) continue;
            AdsbManState st; st.hex = hex; st.callsign = "TST123"; st.t = t;
            st.ground = s.ground;
            st.hasAlt = !s.ground; st.altFt = std::round((alt + 12 * g(r)) / 25) * 25;   // 25 ft steps
            st.hasVrate = sendVrate && !s.ground; st.vrateFpm = std::round((s.vrate + 64 * g(r)) / 64) * 64;
            st.hasHeading = true; st.headingDeg = std::fmod(hdg + 0.2 * g(r) + 360, 360);
            st.hasSpeed = true; st.speedKt = s.speed + 2 * g(r);
            st.hasPos = true; st.lat = 54.6; st.lon = 25.2;
            st.squawk = squawk;
            w.add(st, out);
        }
    }
    return out;
}
static int count(const std::vector<AdsbManAlert>& v, const std::string& k, int* sev = nullptr) {
    int n = 0; for (const auto& a : v) if (a.kind == k) { n++; if (sev) *sev = a.severity; } return n;
}

int main() {
    for (unsigned seed = 1; seed <= 5; seed++) {
        const double loss = 0.1 * (seed - 1);   // 0 .. 40 % of the updates lost
        // cruise with a gentle turn: nothing
        auto v = fly({{600, 0, 0, 450, false}, {60, 0, 1.5, 450, false}, {300, -1500, 0, 420, false}}, 36000, seed, loss);
        CHECK(v.empty(), "seed %u: cruise gave %zu alerts (%s)", seed, v.size(), v.empty() ? "" : v[0].text.c_str());
        // emergency squawk: once
        v = fly({{300, 0, 0, 450, false}}, 30000, seed, loss, true, 7700);
        int sev = -1;
        CHECK(count(v, "emergency", &sev) == 1 && sev == 2, "seed %u: 7700 gave %d alerts", seed, count(v, "emergency"));
        // emergency descent: -6500 fpm warning, -9000 fpm alarm
        v = fly({{120, 0, 0, 450, false}, {60, -6500, 0, 450, false}, {120, 0, 0, 300, false}}, 36000, seed, loss);
        CHECK(count(v, "descent", &sev) == 1 && sev == 1, "seed %u: -6500 fpm gave %d descent alerts sev %d", seed, count(v, "descent"), sev);
        v = fly({{120, 0, 0, 450, false}, {60, -9000, 0, 450, false}}, 36000, seed, loss);
        CHECK(count(v, "descent", &sev) == 1 && sev == 2, "seed %u: -9000 fpm gave %d alerts sev %d", seed, count(v, "descent"), sev);
        // the same without a vertical rate: from the altitude
        v = fly({{120, 0, 0, 450, false}, {90, -7000, 0, 450, false}}, 36000, seed, loss, false);
        CHECK(count(v, "descent") == 1, "seed %u: derived rate gave %d descent alerts", seed, count(v, "descent"));
        // a 4000 fpm descent is not reported
        v = fly({{120, 0, 0, 450, false}, {200, -4000, 0, 450, false}}, 36000, seed, loss);
        CHECK(count(v, "descent") == 0, "seed %u: -4000 fpm reported", seed);
        // steep climb
        v = fly({{60, 0, 0, 300, false}, {60, 8000, 0, 300, false}}, 5000, seed, loss);
        CHECK(count(v, "climb") == 1, "seed %u: 8000 fpm climb gave %d", seed, count(v, "climb"));
        // sharp turn 7 deg/s at 300 kt; a rate-one turn (3 deg/s) is not
        v = fly({{60, 0, 0, 300, false}, {40, 0, 7, 300, false}}, 8000, seed, loss);
        CHECK(count(v, "turn") == 1, "seed %u: 7 deg/s turn gave %d", seed, count(v, "turn"));
        v = fly({{60, 0, 0, 300, false}, {120, 0, -3, 300, false}}, 8000, seed, loss);
        CHECK(count(v, "turn") == 0, "seed %u: rate-one turn reported", seed);
        // go-around: approach from 3500 ft at -800 fpm to about 700 ft, then +1800 fpm
        v = fly({{60, 0, 0, 180, false}, {210, -800, 0, 150, false}, {60, 1800, 0, 160, false}}, 3500, seed, loss);
        CHECK(count(v, "go_around", &sev) == 1 && sev == 1, "seed %u: go-around gave %d", seed, count(v, "go_around"));
        // a normal landing, five minutes on the ground, departure: neither go-around nor touch-and-go
        v = fly({{60, 0, 0, 180, false}, {230, -800, 0, 150, false}, {300, 0, 0, 10, true}, {120, 2000, 0, 160, false}}, 3200, seed, loss);
        CHECK(count(v, "go_around") == 0 && count(v, "touch_and_go") == 0, "seed %u: landing + departure gave %d / %d", seed, count(v, "go_around"), count(v, "touch_and_go"));
        // touch-and-go: 20 s on the runway
        v = fly({{60, 0, 0, 120, false}, {150, -700, 0, 80, false}, {20, 0, 0, 70, true}, {90, 900, 0, 80, false}}, 2000, seed, loss);
        CHECK(count(v, "touch_and_go") == 1 && count(v, "go_around") == 0, "seed %u: touch-and-go gave %d (go-around %d)", seed, count(v, "touch_and_go"), count(v, "go_around"));
        // a departure from the ground alone: nothing
        v = fly({{60, 0, 0, 10, true}, {200, 2500, 0, 180, false}}, 0, seed, loss);
        CHECK(v.empty(), "seed %u: departure gave %zu alerts", seed, v.size());
    }
    // cooldown and prune
    AdsbManoeuvreWatch w;
    std::vector<AdsbManAlert> out;
    for (int i = 0; i < 300; i++) { AdsbManState s; s.hex = "ABC123"; s.t = i * 2; s.hasAlt = true; s.altFt = 30000 - i * 200; s.hasVrate = true; s.vrateFpm = -6000; w.add(s, out); }
    CHECK(count(out, "descent") == 1, "a 10 min descent gave %d alerts (cooldown 600 s)", count(out, "descent"));
    w.prune(5000);
    CHECK(w.tracked() == 0, "prune left %zu", w.tracked());
    return testSummary("adsb_manoeuvre");
}
