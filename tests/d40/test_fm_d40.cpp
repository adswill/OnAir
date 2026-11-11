#include "dect2/d40/fm_d40.h"
#include <cstdio>
#include <cstdlib>
#include <random>
using namespace dect2::d40;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static std::string garble(std::string s, std::mt19937& r) { s[r() % 8] = (char)(r() % 256); return s; }
int main() {
    std::mt19937 rng(5);
    // stations: PS reads with 30 % corrupted; PI on 2 frequencies merges; weak one dropped
    std::vector<FmHit> hits;
    FmHit a; a.mhz = 93.9; a.snrDb = 30; a.pi = 0x4201;
    for (int i = 0; i < 10; i++) a.psReads.push_back(i % 3 == 0 ? garble("NOOR FM ", rng) : "NOOR FM ");
    FmHit b = a; b.mhz = 94.1; b.snrDb = 12;
    FmHit c; c.mhz = 101.0; c.snrDb = 20; c.pi = 0x4300; c.psReads = {"RADIO 1 ", "RADIO 1 ", "RADIO 2 "};
    FmHit w; w.mhz = 99.0; w.snrDb = 2; w.pi = 0x4400;
    FmHit u; u.mhz = 88.0; u.snrDb = 15;   // no PI, no PS
    hits = {a, b, c, w, u};
    auto st = nameStations(hits);
    CHECK(st.size() == 3);
    CHECK(st[1].name == "NOOR FM" && st[1].alsoMhz.size() == 1);
    CHECK(st[2].name == "RADIO 1");
    CHECK(st[0].name.empty());
    // single read is not enough for a name
    FmHit one; one.mhz = 90; one.snrDb = 20; one.pi = 1; one.psReads = {"ONLYONCE"};
    CHECK(nameStations({one})[0].name.empty());

    // DX opening
    DxOpening dx;
    for (int i = 0; i < 12; i++) dx.learn(0x4000 + i, 88.0 + i * 0.8);   // home country 4
    for (int i = 0; i < 12; i++) { dx.observe(0x4000 + i, 88.0 + i * 0.8, 25); dx.observe(0x4000 + i, 88.0 + i * 0.8, 25); }
    CHECK(!dx.check().on);
    dx.observe(0x4F00, 90.0, 20);                                  // a new home-country station: no alarm
    CHECK(!dx.check().on);
    for (int k = 0; k < 2; k++) { dx.observe(0x2111, 87.9, 14); dx.observe(0x3222, 90.5, 12); }   // foreign, confirmed
    dx.observe(0x5333, 99.1, 15);                                  // one read only: bit error candidate
    CHECK(!dx.check().on);
    for (int k = 0; k < 2; k++) dx.observe(0x6444, 100.1, 11);
    auto al = dx.check();
    CHECK(al.on && al.pis.size() == 3);
    // noise-only: random PIs seen once each never alarm
    DxOpening dn; for (int i = 0; i < 12; i++) dn.learn(0x4000 + i, 88.0 + i);
    for (int i = 0; i < 400; i++) dn.observe((int)(rng() % 65535) + 1, 87.5 + (rng() % 200) * 0.1, 15);
    CHECK(!dn.check().on || dn.check().pis.size() >= 3);   // random collisions of 2 reads are rare; count the false rate below
    int falseAlarms = 0;
    for (int t = 0; t < 200; t++) {
        DxOpening d2; for (int i = 0; i < 12; i++) d2.learn(0x4000 + i, 88.0 + i);
        for (int i = 0; i < 60; i++) d2.observe((int)(rng() % 65535) + 1, 87.5 + (rng() % 200) * 0.1, 15);
        if (d2.check().on) falseAlarms++;
    }
    std::printf("DX false alarms in 200 noise-only runs: %d\n", falseAlarms);
    CHECK(falseAlarms == 0);

    // SFN follower
    SfnFollower f;
    double cur = 95.0;
    std::vector<SfnFollower::Cand> cs = {{95.0, 10, 0x4201, "NOOR FM "}, {96.2, 18, 0x4201, "NOOR FM "}, {97.0, 25, 0x4201, "OTHER   "}, {98.0, 30, 0x1111, "NOOR FM "}};
    CHECK(f.update(cur, 0x4201, "NOOR FM ", cs) == cur);
    CHECK(f.update(cur, 0x4201, "NOOR FM ", cs) == cur);
    CHECK(f.update(cur, 0x4201, "NOOR FM ", cs) == 96.2);   // third reading: switches, ignoring the other PS and the other PI
    // flapping: alternating 3 dB differences never switch
    SfnFollower g; int sw = 0; double c2 = 95.0;
    for (int i = 0; i < 100; i++) {
        std::vector<SfnFollower::Cand> q = {{95.0, 20.0 + (i % 2 ? 3 : -3), 0x4201, ""}, {96.2, 20.0 + (i % 2 ? -3 : 3), 0x4201, ""}};
        double n = g.update(c2, 0x4201, "", q); if (n != c2) { sw++; c2 = n; }
    }
    CHECK(sw == 0);
    std::printf(fails ? "FAILED\n" : "fm_d40 ok\n");
    return fails ? 1 : 0;
}
