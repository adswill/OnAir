// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Soft combining of repeated ADS-B frames: two copies of the same frame, each too noisy for the CRC, are decoded from the sum of their soft bits.
// Simulated soft bits (+-1 plus Gaussian noise, the confidence is the magnitude), unknown aircraft so no bit repair applies. Decode rate with and
// without the combiner, and random frames that must never be accepted.
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

static AdsbRaw noisy(const Frame& f, double sigma, std::mt19937& rng, double t) {
    std::normal_distribution<double> n(0.0, sigma);
    AdsbRaw r; r.bits = f.bits; r.levelDbfs = -30; r.timeSec = t;
    for (int k = 0; k < f.bits; k++) {
        const int b = (f.b[k >> 3] >> (7 - (k & 7))) & 1;
        const double y = (b ? 1.0 : -1.0) + n(rng);
        if (y > 0) r.bytes[k >> 3] |= (uint8_t)(0x80 >> (k & 7));
        r.conf[k] = (float)std::fabs(y);
    }
    return r;
}

static int run(bool combine, double sigma, int copies, int trials, uint32_t seed) {
    std::mt19937 rng(seed);
    AdsbTracker tr; AdsbOptions o; o.combine = combine; tr.setOptions(o);
    int ok = 0; double t = 10; AdsbFrame out;
    for (int k = 0; k < trials; k++) {
        const Frame f = encodeAirbornePosition(0x400000 + (uint32_t)k, 5, 11, 36000, false, false, 55.2, 25.1);
        bool got = false;
        for (int c = 0; c < copies; c++) {
            AdsbRaw r = noisy(f, sigma, rng, t); t += 0.5;
            if (tr.accept(r, out, true) && !std::memcmp(out.bytes, f.b, 14)) got = true;
        }
        t += 1.0;
        ok += got;
    }
    return ok;
}

int main() {
    const int N = 600;
    for (double sigma : {0.43, 0.5}) {
        const int off2 = run(false, sigma, 2, N, 1), on2 = run(true, sigma, 2, N, 1), on3 = run(true, sigma, 3, N, 1), off3 = run(false, sigma, 3, N, 1);
        printf("sigma %.2f: two copies %d -> %d of %d, three copies %d -> %d (combiner off -> on)\n", sigma, off2, on2, N, off3, on3);
        CHECK(on2 - off2 > N / 4, "sigma %.2f: combining two copies (%d) must add at least a quarter of the frames (%d without)", sigma, on2, off2);
        CHECK(on3 >= on2, "three copies are not worse than two");
    }
    // unrelated frames never combine into something that passes
    std::mt19937 rng(7);
    AdsbTracker tr; AdsbFrame out; int falseOk = 0; double t = 10;
    for (int k = 0; k < 300000; k++) {
        AdsbRaw r; r.bits = 112; r.timeSec = t; t += 1e-3;
        for (auto& b : r.bytes) b = (uint8_t)rng();
        r.bytes[0] = (uint8_t)((r.bytes[0] & 7) | (17 << 3));
        for (auto& c : r.conf) c = (rng() % 100) * 0.1f;
        if (tr.accept(r, out, true)) falseOk++;
    }
    printf("random frames accepted: %d of 300000\n", falseOk);
    CHECK(falseOk == 0, "random frames accepted: %d", falseOk);
    return testSummary("adsb_combine");
}
