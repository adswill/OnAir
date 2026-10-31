// HFDL band hopping: finds the band that works, stays on it, follows a change of propagation, uses the announced frequencies,
// tours every band when all is silent, ignores a user's tuning off the HFDL bands, and the band table covers the station table.
#include "dect2/hfdl/hfdl_hop.h"
#include "dect2/hfdl/hfdl_proto.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <set>
using namespace dect2;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

struct Sim {
    HfdlBandHopper h;
    std::mt19937 rng;
    int radioBand = 2;              // where the radio is
    int target = -1; double tuneAt = 0;   // a retune in progress (the radio needs a moment)
    uint64_t good = 0;
    std::set<int> visited;
    int hops = 0;
    std::vector<std::pair<double, int>> log;   // (time, band) of every arrival
    std::function<double(double, int)> perMin;   // true PDUs per minute on a band at a time
    std::function<std::vector<int>(double, int)> announce;
    explicit Sim(unsigned seed) : rng(seed) { visited.insert(2); log.push_back({0, 2}); }
    void run(double t0, double t1) {
        for (double t = t0; t < t1; t += 1.0) {
            if (target >= 0 && t >= tuneAt) { radioBand = target; target = -1; visited.insert(radioBand); log.push_back({t, radioBand}); }
            if (target < 0 && radioBand >= 0) {   // the radio listens: Poisson PDUs on the current band
                std::poisson_distribution<int> p(std::max(0.0, perMin(t, radioBand)) / 60.0);
                good += (uint64_t)p(rng);
            }
            HfdlBandHopper::Obs o; o.nowSec = t; o.band = target >= 0 ? -1 : radioBand; o.goodPdus = good;
            if (announce && radioBand >= 0) o.announcedKhz = announce(t, radioBand);
            const int b = h.update(o);
            if (b >= 0) { target = b; tuneAt = t + 2; hops++; }
        }
    }
    double timeOnBand(int band, double t0, double t1) const {   // from the arrival log
        double on = 0;
        for (size_t i = 0; i < log.size(); i++) {
            const double a = std::max(t0, log[i].first), z = std::min(t1, i + 1 < log.size() ? log[i + 1].first : t1);
            if (log[i].second == band && z > a) on += z - a;
        }
        return on;
    }
};

int main() {
    // the band table covers every frequency of the station table
    {
        int n = 0, miss = 0;
        for (const auto& s : hfdlStations()) for (int k : s.khz) { n++; if (hfdlBandOfKhz(k) < 0) { miss++; printf("not in a band: %d kHz (gs %d)\n", k, s.id); } }
        CHECK(n > 20); CHECK(miss == 0);
        CHECK(hfdlBandOfMhz(8.927) == 2); CHECK(hfdlBandOfMhz(8.90) == 2); CHECK(hfdlBandOfMhz(14.2) == -1); CHECK(hfdlBandOfMhz(21.963) == 7);
    }
    // 1: only the 10 MHz band works (8 PDUs/min); found within a tour, then never left
    for (unsigned seed = 1; seed <= 5; seed++) {
        Sim s(seed); s.perMin = [](double, int b) { return b == 3 ? 8.0 : 0.0; };
        s.run(0, 700);
        CHECK(s.radioBand == 3);
        const double found = s.log.empty() ? 1e9 : [&] { for (auto& e : s.log) if (e.second == 3) return e.first; return 1e9; }();
        CHECK(found < 8 * 60.0);
        const int hopsAtFound = s.hops;
        s.run(700, 3700);   // an hour on the good band: no more hops
        CHECK(s.hops == hopsAtFound); CHECK(s.radioBand == 3);
    }
    // 2: propagation shifts: 8.9 MHz works for 20 min, then dies while 13.3 MHz comes up
    for (unsigned seed = 11; seed <= 15; seed++) {
        Sim s(seed); s.perMin = [](double t, int b) { return t < 1200 ? (b == 2 ? 7.0 : 0.0) : (b == 5 ? 7.0 : b == 2 ? 0.0 : 0.0); };
        s.run(0, 1200);
        CHECK(s.timeOnBand(2, 600, 1200) > 590);   // stayed (settled by then)
        s.run(1200, 3600);
        CHECK(s.radioBand == 5);
    }
    // 3: the squitters of the heard stations announce active frequencies on 17.94 MHz, which is the band that works: the first move goes there
    for (unsigned seed = 21; seed <= 25; seed++) {
        Sim s(seed); s.perMin = [](double, int b) { return b == 6 ? 9.0 : b == 2 ? 0.2 : 0.0; };
        s.announce = [](double, int) { return std::vector<int>{17901, 17919, 17928, 17901}; };   // repeats count once
        s.run(0, 400);
        CHECK(s.radioBand == 6);
        CHECK(s.hops <= 2);
    }
    // 4: everything silent: all eight bands are visited in turn within one tour plus margin, none twice before all once
    {
        Sim s(31); s.perMin = [](double, int) { return 0.0; };
        s.run(0, 8 * 55 + 60);
        CHECK(s.visited.size() == 8);   // the eight common bands, not the rare ones
        std::set<int> firstEight; for (size_t i = 0; i < s.log.size() && firstEight.size() < 8; i++) { CHECK(!firstEight.count(s.log[i].second) || s.log[i].second == 2); firstEight.insert(s.log[i].second); }
    }
    // 5: a steady 6 PDUs/min with Poisson noise: no flapping over two hours
    {
        Sim s(41); s.perMin = [](double, int b) { return b == 2 ? 6.0 : 0.0; };
        s.run(0, 7200);
        CHECK(s.hops == 0);
    }
    // 6: a weak but real band (1.5/min) is kept over silent ones once found, and an unvisited better guess is only tried when this one is poor
    {
        Sim s(51); s.perMin = [](double, int b) { return b == 4 ? 1.5 : 0.0; };
        s.run(0, 1500);
        CHECK(s.timeOnBand(4, 900, 1500) > 450);   // most of the time on the only band with traffic
    }
    // 7: the user tunes off the HFDL bands: nothing is returned and nothing breaks; coming back starts a fresh look; counters restarting are handled
    {
        HfdlBandHopper h;
        HfdlBandHopper::Obs o; o.nowSec = 0; o.band = 2; o.goodPdus = 100;
        CHECK(h.update(o) == -1);
        o.nowSec = 5; o.band = -1; CHECK(h.update(o) == -1);
        o.nowSec = 10; o.band = 4; o.goodPdus = 3; CHECK(h.update(o) == -1);   // counter went back
        o.nowSec = 20; o.goodPdus = 3; CHECK(h.update(o) == -1);
        CHECK(!h.status().empty());
        h.reset(); o.nowSec = 0; o.band = 99; CHECK(h.update(o) == 99 ? false : true);   // an out of range band index must not crash
    }
    printf(fails ? "hfdl_hop: %d FAILED\n" : "hfdl_hop: all ok\n", fails);
    return fails ? 1 : 0;
}
