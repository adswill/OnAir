// HFDL "Busiest band" (HfdlBusiestBand, hfdl_hop.h) against a simulated sky: Poisson PDUs per band, each from one of the band's aircraft, the band's
// ground stations' squitters every 32 s (some lost), a retune that takes 1 - 5 s. Checks: it finds the busiest band where "Find a working band"
// (HfdlBandHopper) stays on the first one that works (the user's report), the hints shorten the survey, a band that dies is left and the new
// busiest one found, a silent sky is surveyed over and over with no stay, the user's tuning is respected, off-band tuning, counters restarting
// and a radio that does not retune. Simulated only: the real sky has fading and bursts this does not model.
#include "dect2/hfdl/hfdl_hop.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

struct Sky {                                   // what each band carries at a time
    std::function<double(double, int)> perMin = [](double, int) { return 0.0; };
    std::function<int(double, int)> aircraft = [](double, int) { return 0; };
    std::function<int(double, int)> stations = [](double, int) { return 0; };
    std::function<float(double, int)> snr = [](double, int) { return 10.f; };
};

template <class Hopper>
struct Sim {
    Hopper h;
    Sky sky;
    std::mt19937 rng;
    int radioBand = 2;
    int target = -1; double tuneAt = 0;
    bool radioDeaf = false;                    // the radio ignores retunes
    uint64_t good = 0;
    float lastSnr = 0;
    std::map<uint32_t, double> ac, gs;         // key -> last heard (the telemetry keeps everything heard)
    int hops = 0;
    std::vector<std::pair<double, int>> log;
    std::vector<int> hints;
    explicit Sim(unsigned seed, int start = 2) : rng(seed), radioBand(start) { log.push_back({0, start}); }
    void listen(double t) {
        if (radioBand < 0) return;
        std::poisson_distribution<int> p(std::max(0.0, sky.perMin(t, radioBand)) / 60.0);
        const int k = p(rng);
        const int na = sky.aircraft(t, radioBand);
        for (int i = 0; i < k; i++) {
            good++;
            lastSnr = sky.snr(t, radioBand) + std::normal_distribution<float>(0, 2)(rng);
            if (na > 0) ac[(uint32_t)(radioBand * 1000 + std::uniform_int_distribution<int>(0, na - 1)(rng))] = t;
        }
        const int ns = sky.stations(t, radioBand);
        for (int s = 0; s < ns; s++)   // a squitter every 32 s from each station, 70 % decoded
            if (std::fmod(t + 7 * s, 32.0) < 1.0 && std::uniform_real_distribution<double>(0, 1)(rng) < 0.7) { gs[(uint32_t)(radioBand * 100 + s)] = t; good++; }
    }
    void run(double t0, double t1);
    double timeOn(int band, double t0, double t1) const {
        double on = 0;
        for (size_t i = 0; i < log.size(); i++) {
            const double a = std::max(t0, log[i].first), z = std::min(t1, i + 1 < log.size() ? log[i + 1].first : t1);
            if (log[i].second == band && z > a) on += z - a;
        }
        return on / (t1 - t0);
    }
    std::set<int> visited() const { std::set<int> v; for (auto& e : log) v.insert(e.second); return v; }
    void retuned(double t, int b) {
        if (b < 0) return;
        hops++;
        if (radioDeaf) return;
        target = b; tuneAt = t + std::uniform_real_distribution<double>(1, 5)(rng);
    }
    void arrive(double t) { if (target >= 0 && t >= tuneAt) { radioBand = target; target = -1; log.push_back({t, radioBand}); } }
};

template <>
void Sim<HfdlBusiestBand>::run(double t0, double t1) {
    for (double t = t0; t < t1; t += 1.0) {
        arrive(t);
        if (target < 0) listen(t);
        HfdlBusiestBand::Obs o;
        o.nowSec = t; o.band = target >= 0 ? -1 : radioBand; o.goodPdus = good; o.snrDb = lastSnr; o.hintKhz = hints;
        for (auto& [k, v] : ac) o.aircraft.push_back({k, v});
        for (auto& [k, v] : gs) o.stations.push_back({k, v});
        retuned(t, h.update(o));
    }
}

template <>
void Sim<HfdlBandHopper>::run(double t0, double t1) {
    for (double t = t0; t < t1; t += 1.0) {
        arrive(t);
        if (target < 0) listen(t);
        HfdlBandHopper::Obs o; o.nowSec = t; o.band = target >= 0 ? -1 : radioBand; o.goodPdus = good;
        retuned(t, h.update(o));
    }
}

// the user's report: 8.9 MHz gives a little (one station, one aircraft, 5 PDU/min), 11.3 MHz is busy (three stations, a dozen aircraft)
static Sky reportSky() {
    Sky s;
    s.perMin = [](double, int b) { return b == 2 ? 5.0 : b == 4 ? 30.0 : 0.0; };
    s.aircraft = [](double, int b) { return b == 2 ? 1 : b == 4 ? 12 : 0; };
    s.stations = [](double, int b) { return b == 2 ? 1 : b == 4 ? 3 : 0; };
    return s;
}

int main() {
    // 0: the score
    {
        HfdlBusiestBand b;
        HfdlBusiestBand::Score s; s.perMin = 10; s.aircraft = 4; s.stations = 2; s.snrDb = 12;
        CHECK(std::fabs(b.scoreOf(s) - (10 + 8 + 6 + 1.2)) < 1e-9);
        s.perMin = 0; s.snrDb = 30; CHECK(std::fabs(b.scoreOf(s) - (8 + 6)) < 1e-9);   // SNR counts only with PDUs
        s.snrDb = -5; s.perMin = 1; CHECK(std::fabs(b.scoreOf(s) - (1 + 8 + 6)) < 1e-9);
    }
    // 1: the report. "Find a working band" stays on 8.9 MHz; "Busiest band" finds 11.3 MHz and spends most of the time there
    for (unsigned seed = 1; seed <= 5; seed++) {
        Sim<HfdlBandHopper> old(seed); old.sky = reportSky();
        old.run(0, 5400);
        CHECK(old.timeOn(2, 600, 5400) > 0.95);
        Sim<HfdlBusiestBand> s(seed); s.sky = reportSky();
        s.run(0, 5400);
        CHECK(s.h.bestBand() == 4);
        CHECK(s.visited().size() == 8);                   // no hints: every common band surveyed
        CHECK(s.timeOn(4, 900, 5400) > 0.6);              // rescans every 20 min cost ~ 7 x 75 s each
        CHECK(s.h.score(4).score > 3 * s.h.score(2).score);
        if (seed == 1) printf("report sky: old hopper %.0f %% on 8.9 MHz, busiest %.0f %% on 11.3 MHz (15-90 min)\n", 100 * old.timeOn(2, 600, 5400), 100 * s.timeOn(4, 900, 5400));
    }
    // 2: with hints (the nearby stations' active frequencies on 8.9 and 11.3 MHz) only those bands are surveyed: more time on the busy one
    for (unsigned seed = 11; seed <= 15; seed++) {
        Sim<HfdlBusiestBand> s(seed, 6); s.sky = reportSky();
        s.hints = {11348, 8885, 8942};
        s.run(0, 5400);
        CHECK(s.visited() == std::set<int>({2, 4, 6}));   // the start band, then the hinted ones
        CHECK(s.h.bestBand() == 4);
        CHECK(s.timeOn(4, 600, 5400) > 0.8);
        const auto c = s.h.candidates();
        CHECK(c.size() == 2 && c[0] == 4 && c[1] == 2);   // at the last survey: where it was, then the other hinted band
    }
    // 3: propagation changes: 11.3 MHz dies after an hour while 17.94 MHz opens; the new busiest band is found within one survey
    for (unsigned seed = 21; seed <= 25; seed++) {
        Sim<HfdlBusiestBand> s(seed);
        s.sky.perMin = [](double t, int b) { return t < 3600 ? (b == 4 ? 25.0 : b == 2 ? 4.0 : 0.0) : (b == 6 ? 25.0 : b == 2 ? 4.0 : 0.0); };
        s.sky.aircraft = [](double t, int b) { return (t < 3600 ? b == 4 : b == 6) ? 10 : b == 2 ? 1 : 0; };
        s.sky.stations = [](double t, int b) { return (t < 3600 ? b == 4 : b == 6) ? 2 : b == 2 ? 1 : 0; };
        s.run(0, 3600);
        CHECK(s.h.bestBand() == 4);
        s.run(3600, 7200);
        CHECK(s.h.bestBand() == 6);
        CHECK(s.timeOn(6, 3600 + 900, 7200) > 0.6);
    }
    // 4: a silent sky (noise only): surveyed over and over, never a stay, no band scores
    for (unsigned seed = 31; seed <= 33; seed++) {
        Sim<HfdlBusiestBand> s(seed);
        s.run(0, 3600);
        CHECK(s.h.phase() == HfdlBusiestBand::Phase::Survey);
        CHECK(s.visited().size() == 8);
        CHECK(s.hops >= 3600 / 90);
        for (int b = 0; b < 8; b++) CHECK(s.h.score(b).score == 0);
    }
    // 5: the user tunes by hand to 6.6 MHz (some traffic) while it stays on 11.3: it stays on the user's band until the next survey
    {
        Sim<HfdlBusiestBand> s(41); s.sky = reportSky();
        s.sky.perMin = [](double, int b) { return b == 2 ? 5.0 : b == 4 ? 30.0 : b == 1 ? 10.0 : 0.0; };
        s.sky.aircraft = [](double, int b) { return b == 2 ? 1 : b == 4 ? 12 : b == 1 ? 3 : 0; };
        s.run(0, 1800);
        CHECK(s.radioBand == 4 && s.h.phase() == HfdlBusiestBand::Phase::Stay);
        s.radioBand = 1; s.log.push_back({1800, 1});
        const int hops0 = s.hops;
        s.run(1800, 1800 + 1150);
        CHECK(s.hops == hops0 && s.radioBand == 1 && s.h.phase() == HfdlBusiestBand::Phase::Stay);
        s.run(1800 + 1150, 1800 + 2400);                  // then the survey finds 11.3 MHz again
        CHECK(s.h.bestBand() == 4 && s.radioBand == 4);
        // tuned by hand to a silent band: left after one window
        s.radioBand = 7; s.log.push_back({4200, 7});
        const int hops1 = s.hops;
        s.run(4200, 4200 + 90);
        CHECK(s.hops == hops1 + 1);
    }
    // 6: off the HFDL bands, counters restarting, a radio that does not retune, an out of range band
    {
        HfdlBusiestBand h;
        HfdlBusiestBand::Obs o; o.nowSec = 0; o.band = -1;
        CHECK(h.update(o) == -1 && h.status() == "not on an HFDL band" && h.phase() == HfdlBusiestBand::Phase::Idle);
        o.band = 99; CHECK(h.update(o) == -1);
        o.band = 2; o.goodPdus = 500;
        CHECK(h.update(o) == -1 && h.phase() == HfdlBusiestBand::Phase::Survey);
        for (int t = 1; t <= 40; t++) { o.nowSec = t; o.goodPdus = 500 + (uint64_t)t; h.update(o); }
        o.goodPdus = 3;                                   // the receiver restarted
        int r = -1;
        for (int t = 41; t <= 80 && r < 0; t++) { o.nowSec = t; o.goodPdus = 3 + (uint64_t)(t - 41); r = h.update(o); }
        CHECK(r >= 0);
        CHECK(std::isfinite(h.score(2).score) && h.score(2).score >= 0 && h.score(2).score < 100);
        Sim<HfdlBusiestBand> s(51); s.sky = reportSky(); s.radioDeaf = true;
        s.run(0, 1800);
        CHECK(s.hops <= 1800 / 75 + 2);                   // one try per window, not one per second
        CHECK(s.radioBand == 2);
    }
    // 7: a retune that is slow (up to 5 s) does not count as the user's tuning: the survey goes on
    {
        Sim<HfdlBusiestBand> s(61); s.sky = reportSky();
        s.run(0, 75 * 3 + 30);
        CHECK(s.h.phase() == HfdlBusiestBand::Phase::Survey && s.visited().size() == 4);
    }
    printf(fails ? "hfdl_busy: %d FAILED\n" : "hfdl_busy: all ok\n", fails);
    return fails ? 1 : 0;
}
