// Deeper error correction of ADS-B frames for known aircraft: 4 and 5 wrong bits among the least certain ones are repaired, and random garbage does not become frames. Simulated bits.
#include <algorithm>
#include <vector>
#include "dect2/adsb/adsb_encode.h"
#include "dect2/adsb/adsb_msg.h"
#include "dect2/adsb/adsb_track.h"
#include <cstdio>
#include <cstring>
#include <random>
using namespace dect2;
using namespace dect2::adsb;
#include "../common/test_util.h"

static AdsbRaw raw(const Frame& f, double t) { AdsbRaw r; std::memcpy(r.bytes, f.b, 14); r.bits = f.bits; for (auto& c : r.conf) c = 10; r.levelDbfs = -30; r.timeSec = t; return r; }

int main() {
    const uint32_t A = 0x4CA111;
    std::mt19937 rng(11);
    AdsbTracker tr; AdsbOptions o; o.fixBits = 3; tr.setOptions(o); tr.setReference(55, 25);
    double t = 10; AdsbFrame out;
    { auto r = raw(encodeAirbornePosition(A, 5, 11, 36000, false, false, 55.2, 25.1), t); CHECK(tr.accept(r, out), "first clean"); t += 0.5; }
    { auto r = raw(encodeAirbornePosition(A, 5, 11, 36000, false, true, 55.2, 25.1), t); CHECK(tr.accept(r, out), "second clean"); t += 0.5; }
    int fixed[6] = {}, tried[6] = {};
    for (int flips = 1; flips <= 5; flips++) for (int k = 0; k < 400; k++) {
        const Frame clean = encodeAirbornePosition(A, 5, 11, 36000, false, k & 1, 55.2, 25.1);
        AdsbRaw r = raw(clean, t); t += 0.01;
        std::vector<int> pos; while ((int)pos.size() < flips) { int p = rng() % 72 + 32; if (std::find(pos.begin(), pos.end(), p) == pos.end()) pos.push_back(p); }   // ME and parity: the address itself stays
        for (int i = 0; i < 112; i++) r.conf[i] = 5 + (rng() % 100) * 0.1f;
        for (int p : pos) { r.bytes[p >> 3] ^= 0x80 >> (p & 7); r.conf[p] = 0.1f + (rng() % 10) * 0.05f; }
        tried[flips]++;
        if (tr.accept(r, out, false) && !std::memcmp(r.bytes, clean.b, 14)) fixed[flips]++;
    }
    for (int f = 1; f <= 5; f++) printf("%d wrong bits, least certain: repaired %d/%d\n", f, fixed[f], tried[f]);
    CHECK(fixed[4] > 380 && fixed[5] > 360, "deep repair");
    // the same frames without the option stop at three
    {
        AdsbTracker t2; AdsbOptions p; p.fixBits = 2; t2.setOptions(p); t2.setReference(55, 25);
        double u = 10; AdsbRaw r0 = raw(encodeAirbornePosition(A, 5, 11, 36000, false, false, 55.2, 25.1), u); t2.accept(r0, out);
        const Frame clean = encodeAirbornePosition(A, 5, 11, 36000, false, true, 55.2, 25.1);
        AdsbRaw r = raw(clean, u + 1);
        for (int p2 : {40, 50, 60, 70}) { r.bytes[p2 >> 3] ^= 0x80 >> (p2 & 7); r.conf[p2] = 0.2f; }
        CHECK(!t2.accept(r, out, false), "fixBits 2 does not try four");
    }
    // false repairs: random frames that claim the known address, with random confidences
    int falseOk = 0; const int N = 200000;
    {
        for (int k = 0; k < N; k++) {
            AdsbRaw r; r.bits = 112; r.levelDbfs = -30; r.timeSec = t; t += 1e-5;
            for (auto& b : r.bytes) b = (uint8_t)rng();
            r.bytes[0] = 0x8D; r.bytes[1] = A >> 16; r.bytes[2] = A >> 8; r.bytes[3] = A;
            for (auto& c : r.conf) c = (rng() % 100) * 0.1f;
            if (tr.accept(r, out, false)) falseOk++;
        }
    }
    printf("false repairs of %d random frames carrying a known address: %d\n", N, falseOk);
    CHECK(falseOk <= N / 2000, "false repairs %d", falseOk);
    return testSummary("adsb_deeprepair");
}
