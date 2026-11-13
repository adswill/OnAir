// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Coherent group of independent receivers: start offsets and phase/gain errors recovered from a noise-source calibration, then MUSIC bearing
// accuracy on the corrected data (synthetic 5-element UCA, known bearings), plus a refusal of incoherent channels.
#include "dect2/df/group.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace dect2::df;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const double FREQ = 433e6, LAM = kC / FREQ;

struct Feed : ChannelFeed {
    std::vector<cd> d; size_t p = 0;
    size_t read(cd* o, size_t n) override { size_t k = std::min(n, d.size() - p); for (size_t i = 0; i < k; i++) o[i] = d[p + i]; p += k; return k; }
};
static SimConfig base(const Array& a, uint64_t seed) {
    SimConfig c; c.array = a; c.freqHz = FREQ; c.sampleRate = 250e3; c.seed = seed;
    for (int k = 0; k < a.n(); k++) c.elementGains.push_back(std::polar(0.7 + 0.1 * k, 0.9 * k - 1.7));
    return c;
}

int main() {
    Array arr = Array::uca(5, Array::ucaRadiusForSpacing(5, LAM, 0.5));
    const std::vector<int> lead = {5, 0, 11, 3, 8};   // each device starts this many samples early (junk in front)
    const size_t NOISE = 16384;
    double worst = 0;
    for (int trial = 0; trial < 8; trial++) {
        double b = 360.0 * (trial + 0.3) / 8;
        SimConfig cn = base(arr, 10 + trial); cn.calibrationNoise = true;
        SimMultiChannelSource sn(cn); Frame fn; sn.read(fn, NOISE);
        SimConfig cs = base(arr, 50 + trial); SimSource s; s.bearingDeg = b; s.snrDb = 10; cs.sources.push_back(s);
        SimMultiChannelSource ss(cs); Frame fs; ss.read(fs, 8192);
        Rng rng(77 + trial);
        std::vector<std::unique_ptr<ChannelFeed>> v;
        for (int c = 0; c < 5; c++) {
            auto f = std::make_unique<Feed>();
            for (int i = 0; i < lead[c]; i++) f->d.push_back(cd(rng.gauss(), rng.gauss()));
            f->d.insert(f->d.end(), fn[c].begin(), fn[c].end());
            f->d.insert(f->d.end(), fs[c].begin(), fs[c].end());
            v.push_back(std::move(f));
        }
        CoherentGroup g(std::move(v), 250e3, FREQ);
        CHECK(g.calibrate(8192, 32), "calibration failed (trial %d, coherence %.3f)", trial, g.calibration().coherence);
        for (int c = 0; c < 5; c++) CHECK(g.calibration().lagSamples[c] == lead[c] - lead[0], "trial %d lag ch%d = %d", trial, c, g.calibration().lagSamples[c]);
        Frame junk; g.read(junk, NOISE - g.calibration().consumed + 64);   // rest of the noise block + a guard
        Frame x; g.read(x, 4096);
        Options o; o.method = Method::Music; o.sources = 1;
        Result r = estimate(covariance(x), arr, LAM, o);
        worst = std::max(worst, std::abs(angDiff(r.bearingDeg, b)));
    }
    printf("group: worst MUSIC error %.2f deg\n", worst);
    CHECK(worst < 2.0, "bearing error %.2f", worst);
    {   // independent noise on every channel is not coherent
        Rng r(5); std::vector<std::unique_ptr<ChannelFeed>> v;
        for (int c = 0; c < 4; c++) { auto f = std::make_unique<Feed>(); for (int i = 0; i < 20000; i++) f->d.push_back(cd(r.gauss(), r.gauss())); v.push_back(std::move(f)); }
        CoherentGroup g(std::move(v), 250e3, FREQ);
        CHECK(!g.calibrate(8192, 16), "independent noise accepted as coherent");
    }
    printf(fails ? "df_group: %d FAILED\n" : "df_group: all passed\n", fails);
    return fails ? 1 : 0;
}
