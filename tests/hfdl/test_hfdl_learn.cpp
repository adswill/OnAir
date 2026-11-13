// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HFDL ground-station frequency learning (hfdl_learn.h) against a synthetic system table that changes: the masks read against the old built-in table go
// wrong, the learner must follow what is heard.
#include "dect2/hfdl/hfdl_learn.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <set>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

using Table = std::map<int, std::vector<int>>;
static const Table kBuiltin = {
    {1, {2998, 5720, 8912, 11184, 13264, 17919}},
    {2, {3455, 5720, 6532, 10027, 11321, 15025}},
    {3, {2992, 5529, 8927, 10081, 11348, 13324, 21934}},
    {4, {3016, 5502, 8942, 10084, 13321, 17916}},
    {5, {3007, 6529, 8825, 10060, 11306, 13270}},
    {6, {2941, 5538, 6619, 10066, 11327, 17934}},
};

static bool has(const std::vector<int>& v, int k) { return std::find(v.begin(), v.end(), k) != v.end(); }

// the squitter the station sends on dial frequency f when its table is `table` and `active` is the set in use
static std::vector<int> maskAgainstBuiltin(int gs, const std::vector<int>& table, const std::set<int>& active) {
    // the station sets bit i for the i-th frequency of ITS table
    std::vector<int> bits;
    for (size_t i = 0; i < table.size(); i++) if (active.count(table[i])) bits.push_back((int)i);
    // the receiver reads bit i against the built-in table
    std::vector<int> out;
    const auto& b = kBuiltin.at(gs);
    for (int i : bits) if ((size_t)i < b.size()) out.push_back(b[(size_t)i]);
    return out;
}

struct Sim {
    HfdlFreqLearner learner;
    std::map<int, std::vector<int>> truthTable;           // the tables on the air (start = built-in)
    std::map<int, int> version;
    std::map<int, std::set<int>> active;                  // frequencies in use now
    std::mt19937 rng{1};
    double loss = 0.4;
    Sim() { learner.setBuiltin(kBuiltin); truthTable = kBuiltin; for (auto& kv : kBuiltin) { version[kv.first] = 50; for (int k : kv.second) active[kv.first].insert(k); } }
    // listens to one band at a time: the receiver is on the band of `listenKhz` (a window of 600 kHz)
    void run(double t0, double t1, double dt, std::vector<int> (*bandAt)(double)) {
        for (double t = t0; t < t1; t += dt) {
            const std::vector<int> on = bandAt(t);
            learner.listen(on, dt);
            for (auto& kv : truthTable) {
                const int gs = kv.first;
                for (int f : active[gs]) {
                    if (!has(on, f)) continue;
                    // a squitter every 32 s on each frequency
                    const double p = dt / 32.0;
                    if (std::uniform_real_distribution<double>(0, 1)(rng) > p) continue;
                    if (std::uniform_real_distribution<double>(0, 1)(rng) < loss) continue;
                    learner.squitter(gs, f, version[gs], maskAgainstBuiltin(gs, kv.second, active[gs]), t);
                }
            }
        }
    }
};

static std::vector<int> bandRound(double t) {   // eight bands, 10 minutes each, round robin
    static const int lo[8] = {2800, 3400, 5400, 6500, 8800, 10000, 11200, 13200};
    static const int hi[8] = {3100, 3600, 5800, 6700, 9000, 10200, 11400, 13400};
    const int b = ((int)(t / 600)) % 8;
    std::vector<int> v;
    for (const auto& kv : kBuiltin) for (int k : kv.second) if (k >= lo[b] && k <= hi[b]) v.push_back(k);
    // learned frequencies outside the known bands are not visited here
    return v;
}
static std::vector<int> bandsAll(double) {   // a wide capture: every frequency
    std::vector<int> v;
    for (const auto& kv : kBuiltin) for (int k : kv.second) v.push_back(k);
    v.push_back(9999); v.push_back(5123);
    return v;
}

static void scenario() {
    Sim s;
    const double day = 86400;
    s.run(0, day, 10, bandsAll);                         // a quiet day: nothing changes
    for (const auto& r : s.learner.rows()) CHECK(!r.drift && r.learned.empty() && r.retired.empty(), "no change, no drift (gs %d)", r.gsId);
    CHECK(s.learner.allPlanKhz().size() > 30, "plan is the built-in one");
    // station 3: new table (version 51): 11348 dropped, 9999 added, the others in a new order (the masks now point at the wrong frequencies)
    s.truthTable[3] = {5529, 2992, 8927, 9999, 10081, 13324, 21934};
    s.version[3] = 51;
    s.active[3] = {5529, 2992, 8927, 9999, 10081, 13324, 21934};
    // station 5: only the version number changes (a note), same frequencies
    s.version[5] = 52;
    const double t1 = day;
    s.run(t1, t1 + 1800, 10, bandsAll);                  // 30 minutes after: not enough listening to drop anything
    CHECK(has(s.learner.plan(3), 11348), "11348 not dropped after 30 minutes");
    CHECK(has(s.learner.plan(3), 9999), "9999 learned");
    CHECK(s.learner.drifted(3), "drift flagged");
    s.run(t1 + 1800, t1 + 4 * 3600, 10, bandsAll);
    const auto p3 = s.learner.plan(3);
    CHECK(!has(p3, 11348), "11348 dropped after the listening time");
    CHECK(has(p3, 9999) && has(p3, 5529) && has(p3, 8927) && has(p3, 21934), "kept the rest");
    CHECK(s.learner.plan(5).size() == kBuiltin.at(5).size(), "station 5 (version note only) loses nothing: %zu", s.learner.plan(5).size());
    for (int g = 1; g <= 6; g++) if (g != 3 && g != 5) CHECK(s.learner.plan(g).size() == kBuiltin.at(g).size(), "station %d unchanged", g);
    // what the hopper is told: for station 3 the heard frequencies, for the others the masks
    const double now = t1 + 4 * 3600;
    const auto ann3 = s.learner.announced(3, maskAgainstBuiltin(3, s.truthTable[3], s.active[3]), now);
    int right = 0;
    for (int k : ann3) if (s.active[3].count(k)) right++;
    const auto naive = maskAgainstBuiltin(3, s.truthTable[3], s.active[3]);
    int naiveRight = 0;
    for (int k : naive) if (s.active[3].count(k)) naiveRight++;
    printf("station 3 after its table changed: announced %zu frequencies, %d right (precision %.0f%%, recall %.0f%%); the old mask reading gave %zu, %d right (precision %.0f%%, recall %.0f%%)\n",
           ann3.size(), right, ann3.empty() ? 0.0 : 100.0 * right / ann3.size(), 100.0 * right / s.active[3].size(),
           naive.size(), naiveRight, naive.empty() ? 0.0 : 100.0 * naiveRight / naive.size(), 100.0 * naiveRight / s.active[3].size());
    CHECK(right == (int)ann3.size() && right >= (int)s.active[3].size() - 1, "announced list is what is on the air");
    CHECK(naiveRight < (int)s.active[3].size(), "the old reading was wrong, so the test means something");
    // save and load
    HfdlFreqLearner l2;
    l2.setBuiltin(kBuiltin);
    l2.load(s.learner.save());
    CHECK(l2.plan(3) == s.learner.plan(3) && l2.plan(1) == s.learner.plan(1), "save and load");
    l2.load("hfdllearn 1\nG 999 5 1 1 1 1\nF 3 100 1 1 1 1\nF 3 9000 x\n");
    CHECK(l2.plan(3) == s.learner.plan(3), "damaged lines skipped");
}

static void guards() {
    // a station renumbered but still on every frequency: nothing dropped even after a long time
    {
        Sim s;
        s.run(0, 3600, 10, bandsAll);
        s.truthTable[1] = {5720, 2998, 8912, 11184, 13264, 17919};   // same set, new order
        s.version[1] = 51;
        s.run(3600, 3600 + 6 * 3600, 10, bandsAll);
        CHECK(s.learner.plan(1).size() == 6, "renumbering alone drops nothing");
    }
    // the receiver never listens to the dropped frequency: nothing is dropped (no evidence)
    {
        Sim s;
        s.run(0, 3600, 10, bandsAll);
        s.truthTable[4] = {3016, 5502, 8942, 10084, 13321};          // 17916 dropped
        s.version[4] = 51; s.active[4] = {3016, 5502, 8942, 10084, 13321};
        s.run(3600, 3600 + 6 * 3600, 10, [](double) { std::vector<int> v = {3016, 5502, 8942, 10084, 13321, 2998, 3455}; return v; });
        CHECK(has(s.learner.plan(4), 17916), "not listened to: not dropped");
    }
    // a frequency heard once on a station is not learned, twice is
    {
        HfdlFreqLearner l;
        l.setBuiltin(kBuiltin);
        l.squitter(2, 7777, 50, {}, 100);
        CHECK(!has(l.plan(2), 7777), "one squitter is not enough");
        l.squitter(2, 7777, 50, {}, 130);
        CHECK(has(l.plan(2), 7777), "two are");
    }
    // implausible input is refused
    {
        HfdlFreqLearner l;
        l.setBuiltin(kBuiltin);
        l.squitter(0, 5720, 50, {}, 1); l.squitter(-4, 5720, 50, {}, 1); l.squitter(2, 100, 50, {}, 1); l.squitter(2, 50000, 50, {}, 1); l.squitter(2, 5720, 50, {}, NAN);
        l.listen({5720}, -3); l.listen({}, 3); l.listen({5720}, 1e9);
        CHECK(l.rows().empty(), "bad input leaves nothing");
    }
    // a changed table that is heard again on the built-in frequency brings it back
    {
        Sim s;
        s.run(0, 3600, 10, bandsAll);
        s.truthTable[6] = {2941, 5538, 6619, 10066, 11327};          // 17934 dropped
        s.version[6] = 51; s.active[6] = {2941, 5538, 6619, 10066, 11327};
        s.run(3600, 3600 + 4 * 3600, 10, bandsAll);
        CHECK(!has(s.learner.plan(6), 17934), "dropped");
        s.truthTable[6] = {2941, 5538, 6619, 10066, 11327, 17934};
        s.version[6] = 52; s.active[6].insert(17934);
        s.run(3600 + 4 * 3600, 3600 + 5 * 3600, 10, bandsAll);
        CHECK(has(s.learner.plan(6), 17934), "back on the air, back in the plan");
    }
}

// the round-robin tour: how long until a changed station's new frequency is learned, and the false-drop count over a week with no change at all
static void sweep() {
    int falseDrops = 0, runs = 0;
    for (int seed = 0; seed < 6; seed++)
        for (double loss : {0.2, 0.5, 0.8}) {
            Sim s; s.rng.seed(seed + 1); s.loss = loss;
            // a note-only version change at random times, and a different one per station: nothing is ever dropped
            for (int d = 0; d < 7; d++) {
                for (auto& kv : s.version) if ((d + kv.first) % 3 == 0) kv.second++;
                s.run(d * 86400.0, (d + 1) * 86400.0, 20, bandRound);
            }
            for (const auto& r : s.learner.rows()) { falseDrops += (int)r.retired.size(); }
            runs++;
        }
    printf("a week with version changes but no frequency changes (%d runs, 20 to 80 %% of the squitters lost): %d frequencies wrongly dropped\n", runs, falseDrops);
    CHECK(falseDrops == 0, "false drops");
}

int main() {
    scenario();
    guards();
    sweep();
    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
