// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AIS anomaly engine (ais_anomaly.h): clean traffic with GPS noise, lost reports and ships leaving the covered sea must raise nothing; injected spoofing,
// clones, moved aids, wrong classes, impossible speeds, renamed ships and ships that go dark inside coverage must be found.
#include "dect2/ais/ais_anomaly.h"
#include "dect2/ais/ais_alerts.h"
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <set>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const double kOwnLat = 54.0, kOwnLon = 12.0;

static void move(double& lat, double& lon, double sog, double cog, double sec) {
    const double nm = sog * sec / 3600.0, r = M_PI / 180;
    lat += nm * std::cos(cog * r) / 60.0;
    lon += nm * std::sin(cog * r) / (60.0 * std::cos(lat * r));
}
static AisVessel vessel(uint32_t mmsi, double lat, double lon, float sog, float cog, int cls = AIS_CLASS_A, int type = 70, const char* name = "SHIP") {
    AisVessel v; v.mmsi = mmsi; v.hasPos = true; v.lat = lat; v.lon = lon; v.sog = sog; v.cog = cog; v.cls = cls; v.shipType = type; v.name = name; v.navStatus = 0;
    return v;
}
static size_t count(const std::vector<AisAnomaly>& v, const char* kind, uint32_t mmsi = 0) {
    size_t n = 0;
    for (const auto& a : v) if (a.kind == kind && (!mmsi || a.mmsi == mmsi)) n++;
    return n;
}

struct Sim {
    struct Ship { uint32_t mmsi; double lat, lon, sog, cog; int cls, type; double interval, next; bool silent = false; bool quiet = false; };
    std::vector<Ship> ships;
    std::vector<AisAnomaly> all;
    AisAnomalyEngine eng;
    std::mt19937 rng{42};
    double loss = 0.3, gpsNm = 0.005;
    // radio range per 10 degree sector (nm): a soft edge, as a real receiver has
    double rangeOf(double bearing) const { return 18 + 14 * std::sin(bearing * M_PI / 180 * 2 + 1.0) * std::sin(bearing * M_PI / 180); }
    bool heard(double lat, double lon) {
        const double dy = (lat - kOwnLat) * 60, dx = (lon - kOwnLon) * 60 * std::cos(kOwnLat * M_PI / 180);
        const double d = std::sqrt(dx * dx + dy * dy), b = std::fmod(std::atan2(dx, dy) * 180 / M_PI + 360, 360);
        const double r = rangeOf(b);
        const double p = d < r * 0.8 ? 1.0 : d > r ? 0.0 : (r - d) / (0.2 * r);
        return std::uniform_real_distribution<double>(0, 1)(rng) < p;
    }
    void populate(int n) {
        std::uniform_real_distribution<double> u(0, 1);
        for (int k = 0; k < n; k++) {
            Ship s;
            s.mmsi = 219000000 + 1000 + (uint32_t)k;
            const double d = 4 + u(rng) * 22, b = u(rng) * 360;
            s.lat = kOwnLat + d * std::cos(b * M_PI / 180) / 60; s.lon = kOwnLon + d * std::sin(b * M_PI / 180) / 60 / std::cos(kOwnLat * M_PI / 180);
            s.sog = k % 5 == 0 ? 0.0 : 3 + u(rng) * 17; s.cog = u(rng) * 360;
            s.cls = k % 4 == 0 ? AIS_CLASS_B : AIS_CLASS_A;
            const int types[] = {70, 80, 60, 30, 37, 52, 36};
            s.type = types[k % 7];
            s.interval = s.cls == AIS_CLASS_B ? 30 : (s.sog < 0.5 ? 180 : 10);
            s.next = u(rng) * s.interval;
            ships.push_back(s);
        }
    }
    // runs from t0 to t1 in 5 s steps
    void run(double t0, double t1, AisAnomalyConfig cfg) {
        cfg.ownValid = true; cfg.ownLat = kOwnLat; cfg.ownLon = kOwnLon;
        eng.configure(cfg);
        std::normal_distribution<double> g(0, gpsNm / 60);
        for (double t = t0; t < t1; t += 5) {
            for (auto& s : ships) {
                move(s.lat, s.lon, s.sog, s.cog, 5);
                if (s.silent || t < s.next) continue;
                s.next = t + s.interval;
                if (!heard(s.lat, s.lon)) continue;
                if (std::uniform_real_distribution<double>(0, 1)(rng) < loss) continue;
                AisVessel v = vessel(s.mmsi, s.lat + g(rng), s.lon + g(rng), (float)s.sog, (float)s.cog, s.cls, s.type);
                for (auto& a : eng.update(v, t)) all.push_back(a);
            }
        }
    }
};

static AisAnomalyConfig defaults() { return AisAnomalyConfig(); }

static void cleanTraffic() {
    int totalFalse = 0;
    for (int seed = 0; seed < 8; seed++) {
        Sim sim;
        sim.rng.seed(100 + seed);
        sim.loss = 0.1 + 0.1 * seed;                 // 10 % .. 80 % of the reports lost
        sim.gpsNm = 0.002 + 0.004 * seed;            // 4 m .. 55 m of GPS noise
        sim.populate(150);
        sim.run(0, 4 * 3600, defaults());           // ships sail out of the covered sea in this time
        for (const auto& a : sim.all) { printf("  false alarm seed %d: %s %u %s\n", seed, a.kind.c_str(), (unsigned)a.mmsi, a.text.c_str()); totalFalse++; }
    }
    printf("clean traffic: 8 runs x 150 ships x 4 h, loss 10..80 %%, GPS noise to 55 m: %d false alarms\n", totalFalse);
    CHECK(totalFalse == 0, "false alarms on clean traffic");
}

static void feed(AisAnomalyEngine& e, std::vector<AisAnomaly>& out, const AisVessel& v, double t) { for (auto& a : e.update(v, t)) out.push_back(a); }

static void jumpAndClash() {
    // a spoofed single position: one jump, and no clash when the ship goes on from where it was
    {
        AisAnomalyEngine e; std::vector<AisAnomaly> out;
        double lat = 54.0, lon = 12.0, t = 0;
        for (int k = 0; k < 60; k++) { move(lat, lon, 12, 90, 10); t += 10; feed(e, out, vessel(219000001, lat, lon, 12, 90), t); }
        CHECK(out.empty(), "steady track raises nothing");
        feed(e, out, vessel(219000001, lat + 2.0, lon, 12, 90), t + 10);   // 120 nm north
        for (int k = 0; k < 20; k++) { move(lat, lon, 12, 90, 10); t += 10; feed(e, out, vessel(219000001, lat, lon, 12, 90), t + 20); }
        CHECK(count(out, "jump") == 1 && count(out, "clash") == 0, "spoof: jump %zu clash %zu", count(out, "jump"), count(out, "clash"));
    }
    // a ship that is silent for 3 h then reappears 36 nm away at 12 kn is no jump
    {
        AisAnomalyEngine e; std::vector<AisAnomaly> out;
        feed(e, out, vessel(219000002, 54.0, 12.0, 12, 90), 0);
        feed(e, out, vessel(219000002, 54.0, 12.0 + 36.0 / 60 / std::cos(54 * M_PI / 180), 12, 90), 3 * 3600);
        CHECK(out.empty(), "long silence is not a jump");
    }
    // the same report heard on both AIS channels within a second: nothing
    {
        AisAnomalyEngine e; std::vector<AisAnomaly> out;
        feed(e, out, vessel(219000003, 54.0, 12.0, 0, 0), 0);
        feed(e, out, vessel(219000003, 54.0, 12.0, 0, 0), 0.4);
        CHECK(out.empty(), "dual channel");
    }
    // clone: two ships on one MMSI 60 nm apart, alternating: found within a few reports
    {
        AisAnomalyEngine e; std::vector<AisAnomaly> out;
        double t = 0, found = -1;
        for (int k = 0; k < 60; k++) {
            t += 10;
            const bool second = k % 2;
            for (auto& a : e.update(vessel(219000004, second ? 54.0 + 1.0 : 54.0, 12.0, 10, 90), t)) { out.push_back(a); if (a.kind == "clash" && found < 0) found = t; }
        }
        CHECK(count(out, "clash") == 1 && found > 0 && found <= 60, "clone found after %.0f s, %zu clash alerts", found, count(out, "clash"));
        printf("clone detection: after %.0f s (%d reports)\n", found, (int)(found / 10));
    }
    // two ships with one MMSI 3 nm apart: below the separation limit, no clash (a GPS/AIS repeater or two boats of one owner)
    {
        AisAnomalyEngine e; std::vector<AisAnomaly> out;
        double t = 0;
        for (int k = 0; k < 40; k++) { t += 10; feed(e, out, vessel(219000005, 54.0 + (k % 2) * 3.0 / 60, 12.0, 0, 0), t); }
        CHECK(count(out, "clash") == 0, "3 nm apart is not a clash");
    }
    // message 27 positions (a tenth of a minute, a long latency) next to normal ones: no jump
    {
        AisAnomalyEngine e; std::vector<AisAnomaly> out;
        double t = 0;
        std::mt19937 r(5);
        for (int k = 0; k < 100; k++) {
            t += 60;
            AisVessel v = vessel(219000006, 54.0 + 0.0001 * k + ((int)(r() % 100) - 50) / 20000.0, 12.0 + 0.0003 * k, 8, 90);
            v.lrPosition = k % 3 == 0;
            feed(e, out, v, t);
        }
        for (auto& a : out) printf("  %s %s\n", a.kind.c_str(), a.text.c_str());
        CHECK(out.empty(), "mixed long-range positions: %zu alerts", out.size());
    }
}

static void atonAndIdentity() {
    {
        AisAnomalyEngine e; std::vector<AisAnomaly> out;
        std::mt19937 r(3); std::normal_distribution<double> g(0, 0.002 / 60);
        for (int k = 0; k < 200; k++) feed(e, out, vessel(992190001, 54.5 + g(r), 12.5 + g(r), -1, -1, AIS_CLASS_ATON, -1, "BUOY 1"), k * 180.0);
        CHECK(out.empty(), "a buoy with GPS noise stays quiet (%zu)", out.size());
        feed(e, out, vessel(992190001, 54.5 + 0.5 / 60, 12.5, -1, -1, AIS_CLASS_ATON, -1, "BUOY 1"), 40000);
        CHECK(count(out, "aton_moved") == 1, "aton moved 0.5 nm: %zu", count(out, "aton_moved"));
        AisVessel off = vessel(992190002, 54.5, 12.5, -1, -1, AIS_CLASS_ATON, -1, "BUOY 2");
        feed(e, out, off, 0);
        off.lat += 0.5 / 60; off.offPosition = true;
        feed(e, out, off, 100);
        CHECK(count(out, "aton_off_position", 992190002) == 1 && count(out, "aton_moved", 992190002) == 0, "off-position buoy says so");
    }
    {
        AisAnomalyEngine e; std::vector<AisAnomaly> out;
        feed(e, out, vessel(219123456, 54, 12, 5, 0, AIS_CLASS_A), 0);                  // ship MMSI, class A: fine
        feed(e, out, vessel(219123457, 54, 12, 5, 0, AIS_CLASS_B), 0);                  // class B: fine
        feed(e, out, vessel(982190001, 54, 12, 5, 0, AIS_CLASS_B), 0);                  // craft associated with a parent ship, class B: fine
        feed(e, out, vessel(970123456, 54, 12, 0, 0, AIS_CLASS_A), 0);                  // SART: fine
        CHECK(out.empty(), "legal combinations: %zu alerts", out.size());
        feed(e, out, vessel(2190001, 54, 12, 5, 0, AIS_CLASS_A), 0);                    // 00MID number (coast) sending as a ship: 7 digits
        feed(e, out, vessel(2190002, 54, 12, 5, 0, AIS_CLASS_A), 0);
        feed(e, out, vessel(219555555, 54, 12, -1, -1, AIS_CLASS_ATON), 0);             // ship number sending as an aid
        feed(e, out, vessel(992190003, 54, 12, 5, 0, AIS_CLASS_A), 0);                  // aid number sending as a ship
        CHECK(count(out, "mmsi_class") >= 2, "class mismatch %zu", count(out, "mmsi_class"));
    }
    {
        AisAnomalyEngine e; std::vector<AisAnomaly> out;
        for (int k = 0; k < 5; k++) feed(e, out, vessel(219000010, 54, 12, 45, 0, AIS_CLASS_A, 70), k * 10.0);   // cargo ship at 45 kn
        CHECK(count(out, "speed_type") == 1, "cargo at 45 kn");
        out.clear();
        feed(e, out, vessel(219000011, 54, 12, 45, 0, AIS_CLASS_A, 70), 0);
        feed(e, out, vessel(219000011, 54, 12, 10, 0, AIS_CLASS_A, 70), 10);
        feed(e, out, vessel(219000011, 54, 12, 45, 0, AIS_CLASS_A, 70), 20);
        feed(e, out, vessel(219000011, 54, 12, 45, 0, AIS_CLASS_A, 70), 30);
        CHECK(out.empty(), "one glitch is not three in a row");
        for (int k = 0; k < 5; k++) feed(e, out, vessel(219000012, 54, 12 + 0.0001 * k, 38, 90, AIS_CLASS_A, 40), k * 10.0);   // high-speed craft at 38 kn
        feed(e, out, vessel(219000013, 54, 12, 102.3f, 0, AIS_CLASS_A, 70), 0);
        feed(e, out, vessel(219000013, 54, 12, 102.3f, 0, AIS_CLASS_A, 70), 10);
        feed(e, out, vessel(219000013, 54, 12, 102.3f, 0, AIS_CLASS_A, 70), 20);
        CHECK(out.empty(), "HSC fast, 'not available' speed ignored: %zu", out.size());
    }
    {
        AisAnomalyEngine e; std::vector<AisAnomaly> out;
        feed(e, out, vessel(219000020, 54, 12, 5, 0, AIS_CLASS_A, 70, "NORDIC STAR"), 0);
        feed(e, out, vessel(219000020, 54, 12, 5, 0, AIS_CLASS_A, 70, "NORDIC STAR  "), 10);
        feed(e, out, vessel(219000020, 54, 12, 5, 0, AIS_CLASS_A, 70, "NORDIC STAT"), 20);     // one bit error: not news
        feed(e, out, vessel(219000020, 54, 12, 5, 0, AIS_CLASS_A, 70, "NORDIC"), 30);          // shorter: not news
        CHECK(out.empty(), "typo/cut names quiet");
        feed(e, out, vessel(219000020, 54, 12, 5, 0, AIS_CLASS_A, 70, "MV TIGER"), 40);
        CHECK(count(out, "name_change") == 1, "renamed");
    }
}

// dark ships: found when the silence is inside the covered sea, not when the ship left it or was not under way
static void darkShips() {
    int found = 0, wanted = 0, falseDark = 0, total = 0, skipped = 0, foundEligible = 0;
    double sumDelay = 0;
    for (int seed = 0; seed < 6; seed++) {
        Sim sim;
        sim.rng.seed(900 + seed);
        sim.loss = 0.2;
        sim.populate(100);
        // ships 1..4 sit at 3, 6, 10 and 14 nm and move slowly along an arc of a circle; they have 10 s reports
        const double frac[5] = {0.25, 0.35, 0.45, 0.5, 0.92};   // of the range heard on that bearing; the last one is at the edge of coverage
        const int bearings[5] = {20, 110, 200, 290, 160};
        for (int k = 0; k < 5; k++) {
            auto& sh = sim.ships[(size_t)(k + 1)];
            const double rr = frac[k] * sim.rangeOf(bearings[k]);
            sh.lat = kOwnLat + rr * std::cos(bearings[k] * M_PI / 180) / 60;
            sh.lon = kOwnLon + rr * std::sin(bearings[k] * M_PI / 180) / 60 / std::cos(kOwnLat * M_PI / 180);
            sh.sog = 3; sh.cog = bearings[k] + 90; sh.cls = AIS_CLASS_A; sh.interval = 10; sh.next = 0;
        }
        sim.ships[0].sog = 0; sim.ships[0].interval = 180;               // a moored ship that stops transmitting
        sim.ships[0].lat = kOwnLat + 0.03; sim.ships[0].lon = kOwnLon;
        sim.run(0, 3600, defaults());
        const size_t before = sim.all.size();
        for (int k = 0; k <= 5; k++) sim.ships[(size_t)k].silent = true;
        const double tSilent = 3600;
        sim.run(tSilent, tSilent + 3 * 3600, defaults());
        // what the engine said after the silence
        std::set<uint32_t> dark;
        for (size_t i = before; i < sim.all.size(); i++) {
            const auto& a = sim.all[i];
            if (a.kind == "dark") {
                dark.insert(a.mmsi);
                bool ours = false;
                for (int k = 1; k <= 4; k++) if (sim.ships[(size_t)k].mmsi == a.mmsi) { ours = true; sumDelay += a.utc - tSilent; }
                if (sim.ships[5].mmsi == a.mmsi) printf("  edge ship flagged dark: %s\n", a.text.c_str());
                if (!ours) falseDark++;
            }
        }
        for (int k = 1; k <= 4; k++) {
            // eligible: the coverage model knows the sector and the ship was well inside what it heard there (the model never promises more than that)
            const double model = sim.eng.sectorRangeNm(bearings[k - 1]), rr = frac[k - 1] * sim.rangeOf(bearings[k - 1]);
            total++;
            if (model <= 0 || rr > 0.45 * model) { skipped++; if (dark.count(sim.ships[(size_t)k].mmsi)) found++; continue; }
            wanted++;
            if (dark.count(sim.ships[(size_t)k].mmsi)) { found++; foundEligible++; }
            else printf("  seed %d ship %d (bearing %d, %.1f nm): model range there %.1f nm: MISSED\n", seed, k, bearings[k - 1], rr, model);
        }
        CHECK(!dark.count(sim.ships[0].mmsi), "moored ship is not dark");
        for (size_t i = 0; i < before; i++) CHECK(false, "alert before the silence: %s %s", sim.all[i].kind.c_str(), sim.all[i].text.c_str());
    }
    printf("dark ships: %d ships silenced inside coverage, %d where the model knew the sector and the ship was inside 45 %% of its range: found %d (all found, eligible or not: %d), mean delay %.0f min, false dark alerts %d\n", total, wanted, foundEligible, found, found ? sumDelay / found / 60 : 0, falseDark);
    CHECK(wanted > 0 && foundEligible == wanted, "dark ships found %d of %d eligible", foundEligible, wanted);
    (void)skipped;
    // other ships that fall silent (they sailed away) are the 'false dark' count: those were not under our control, check it is small
    CHECK(falseDark <= 6, "false dark %d", falseDark);
    // a ship that returns says so
    {
        AisAnomalyEngine e; std::vector<AisAnomaly> out;
        AisAnomalyConfig c; c.ownValid = true; c.ownLat = kOwnLat; c.ownLon = kOwnLon;
        e.configure(c);
        std::mt19937 r(1); std::uniform_real_distribution<double> u(0, 1);
        double t = 0;
        // fill the coverage model: reports from all around out to 20 nm
        for (int k = 0; k < 4000; k++) { t += 2; const double d = 2 + u(r) * 18, b = u(r) * 360; feed(e, out, vessel(219100000 + (uint32_t)(k % 300), kOwnLat + d * std::cos(b * M_PI / 180) / 60, kOwnLon + d * std::sin(b * M_PI / 180) / 60 / std::cos(kOwnLat * M_PI / 180), 0.4f, 0, AIS_CLASS_A, 70), t); }
        out.clear();
        double lat = kOwnLat + 5.0 / 60, lon = kOwnLon;
        for (int k = 0; k < 20; k++) { move(lat, lon, 8, 90, 10); t += 10; feed(e, out, vessel(219999999, lat, lon, 8, 90), t); }
        const double last = t;
        t += 3000;
        feed(e, out, vessel(219100001, kOwnLat, kOwnLon + 0.1, 0.4f, 0, AIS_CLASS_A, 70), t);   // any report triggers the scan
        CHECK(count(out, "dark", 219999999) == 1, "ship goes dark");
        move(lat, lon, 8, 90, 3000); t += 100;
        feed(e, out, vessel(219999999, lat, lon, 8, 90), t);
        CHECK(count(out, "reappeared", 219999999) == 1, "ship reappears");
        (void)last;
    }
}

int main() {
    cleanTraffic();
    jumpAndClash();
    atonAndIdentity();
    darkShips();
    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
