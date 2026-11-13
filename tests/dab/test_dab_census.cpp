// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB census: three generated ensembles upsampled into one 8.192 Msps capture, noise sweep, offset, adjacent channels. Simulated (DabGenerator): no wide recording on hand.
#include "dect2/dab/dab.h"
#include "dect2/dab/dab_gen.h"
#include "dect2/dab/dab_census.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/test_long.h"

static std::vector<cf32> capture(double secs, double snr, double cfo, unsigned seed, std::vector<std::pair<double, dabgen::TxConfig>> tx) {
    const double rate = 8192000, centre = 226500000;
    const size_t n = (size_t)(secs * rate);
    std::vector<cf32> sum(n, cf32(0, 0)), y(n);
    std::mt19937 rng(seed);
    for (auto& t : tx) {
        SynthConfig sc; sc.snrDb = 200; sc.cfoHz = cfo;
        auto g = makeDabSynth(t.second, sc, rate);
        g->generate(y.data(), n);
        const double w = 2 * M_PI * (t.first - centre) / rate;
        for (size_t i = 0; i < n; i++) sum[i] += y[i] * cf32((float)std::cos(w * i), (float)std::sin(w * i));
    }
    double p = 0; for (auto& v : sum) p += std::norm(v);
    p /= sum.size() * tx.size();                                // the power of one ensemble
    std::normal_distribution<float> g(0, (float)std::sqrt(p * std::pow(10.0, -snr / 10.0) * (rate / 1536000.0) / 2));   // SNR in the ensemble's own band
    for (auto& v : sum) v += cf32(g(rng), g(rng));
    return sum;
}

static dabgen::TxConfig ens(const char* name, uint16_t eid, std::vector<std::pair<uint16_t, const char*>> s, unsigned seed) {
    dabgen::TxConfig c; c.ensembleLabel = name; c.eid = eid; c.seed = seed;
    int sub = 1;
    for (auto& e : s) { dabgen::TxService v; v.sid = e.first; v.label = e.second; v.subId = sub++; c.services.push_back(v); }
    return c;
}

int main() {
    CHECK(dabBandIII().size() == 41, "plan size %zu", dabBandIII().size());
    const double rate = 8192000, centre = 226500000;
    {   DabCensus c; CHECK(!c.configure(2048000, centre), "too narrow"); CHECK(!c.configure(8192000, 100e6), "no channel"); }
    // the default run: each SNR and each offset once (20 dB centred, 12 dB with the offset); ONAIR_TEST_LONG=1 every pair
    for (double snr : {20.0, 12.0}) for (double cfo : {0.0, 1800.0}) {
        if (!testLong() && (snr == 12.0) != (cfo != 0.0)) continue;
        auto x = capture(10, snr, cfo, 3, {{223936000, ens("Alpha", 0xC1, {{0x4001, "News"}, {0x4002, "Pop"}}, 1)},
                                           {225648000, ens("Beta", 0xC2, {{0x4001, "News"}, {0x4003, "Jazz"}}, 2)},  // adjacent, one service shared
                                           {229072000, ens("Gamma", 0xC3, {{0x4004, "Rock"}}, 3)}});
        DabCensus c; CHECK(c.configure(rate, centre), "configure");
        for (size_t i = 0; i < x.size(); i += 65536) c.feed(x.data() + i, std::min<size_t>(65536, x.size() - i));
        const auto r = c.rows();
        int locked = 0; for (auto& e : r) if (e.locked) locked++;
        printf("SNR %.0f CFO %.0f: %zu rows, %d locked:", snr, cfo, r.size(), locked);
        for (auto& e : r) printf(" %s='%s'(%zu, %.1f dB)", e.channel.c_str(), e.ensemble.c_str(), e.services.size(), e.snrDb);
        printf("\n");
        CHECK(locked == 3, "three locked at SNR %.0f CFO %.0f", snr, cfo);
        CHECK(r.size() == 3, "no phantom channel at SNR %.0f", snr);
        auto m = dabCensusMerge(r);
        CHECK(m.size() == 4, "four distinct services, got %zu", m.size());
        for (auto& s : m) if (s.sid == 0x4001) CHECK(s.alsoOn.size() == 1, "News on two ensembles");
    }
    return testSummary("dab_census");
}
