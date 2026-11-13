// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TD-SCDMA: the code tables (sizes, the complex rules, midamble shifts), then the test transmitter into the search: one to three cells,
// every code group tried once over the cases, carrier offsets, noise, sample clock error, a capture starting anywhere in the sub-frame, the
// two SYNC-DL phase quadruples. Checks the SYNC-DL code, the initial cell parameter (from the midamble and the quadruples) and the frame
// parity. Simulated signals only.
#include "dect2/tdscdma/tdscdma.h"
#include "dect2/tdscdma/tdscdma_rx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <random>
#include <vector>
using namespace dect2;
using namespace dect2::tdscdma;
#include "../common/test_util.h"
#include "../common/impair.h"

int main() {
    // tables: SYNC-DL 0 starts B3 = 1011 0011 -> +1 -1 +1 +1; complex (-j)^i from i = 1: -j, -1, +j, +1
    { auto s = syncDl(0); CHECK(s.size() == 64, "sync size");
      CHECK(std::abs(s[0] - cf32(0, -1)) < 1e-6 && std::abs(s[1] - cf32(1, 0)) < 1e-6 && std::abs(s[2] - cf32(0, 1)) < 1e-6 && std::abs(s[3] - cf32(1, 0)) < 1e-6, "SYNC-DL 0 complex");
      // midamble 0 starts B = 1011: +1 -1 +1 +1 times j^i: j, -1 * -1 = ... j, 1, -j, 1
      auto m = basicMidamble(0); CHECK(std::abs(m[0] - cf32(0, 1)) < 1e-6 && std::abs(m[1] - cf32(1, 0)) < 1e-6 && std::abs(m[2] - cf32(0, -1)) < 1e-6 && std::abs(m[3] - cf32(1, 0)) < 1e-6, "midamble 0 complex");
      // shift k of K = 8: m(k)_i = m_(i + (8 - k) 16); m(8) starts at the basic code's start
      auto m8 = midamble(5, 8), m1 = midamble(5, 1), b = basicMidamble(5);
      CHECK(std::abs(m8[0] - b[0]) < 1e-6 && std::abs(m1[0] - b[112]) < 1e-6 && std::abs(m1[16] - b[0]) < 1e-6, "midamble shifts");
      // the SYNC-DL codes are nearly orthogonal; midambles of one group distinguishable at every shift
      double worst = 0; for (int a = 0; a < 32; a++) for (int c = a + 1; c < 32; c++) { std::complex<double> s0 = 0; auto x = syncDl(a), y = syncDl(c); for (int i = 0; i < 64; i++) s0 += std::complex<double>(x[(size_t)i] * std::conj(y[(size_t)i])); worst = std::max(worst, std::abs(s0)); }
      CHECK(worst < 24, "SYNC-DL cross %f", worst); printf("  SYNC-DL worst cross-correlation %.0f of 64\n", worst);
      CHECK(scramblingCode(0)[0] == -1 && scramblingCode(127)[15] == -1 && oddParam(4) == 5 && oddParam(5) == 4, "scrambling / cycling"); }

    struct Case { std::vector<int> params; std::vector<double> amps; double cfo, ppm, snr; int chipOff; bool s2; int sfn0; };
    const Case cases[] = {
        {{0}, {1}, 0, 0, 15, 0, false, 0},
        {{37}, {1}, 2500, 0, 5, 3001, false, 7},
        {{90}, {1}, -6100, 20, 0, -1777, true, 12},
        {{127, 66}, {1, 0.5}, 900, -10, 8, 501, false, 3},
        {{13, 54, 109}, {1, 0.7, 0.5}, -300, 5, 10, 4444, true, 100},
        {{81}, {1}, 4000, 0, -3, 6000, false, 1},
    };
    int ci = 0;
    for (const Case& c : cases) {
        std::vector<TxCell> tx;
        for (size_t k = 0; k < c.params.size(); k++) { TxCell t; t.cellParam = c.params[k]; t.amp = c.amps[k]; t.cfoHz = c.cfo + 50.0 * (double)k; t.chipOffset = c.chipOff + 3 * (int)k; t.s2Every = c.s2; t.sfn0 = c.sfn0; tx.push_back(t); }
        std::vector<cf32> x = generate(tx, (size_t)(kFs * 0.08), c.snr, 3 + (unsigned)ci);
        if (c.ppm != 0) x = impair::clock(x, c.ppm);
        double ms = 0;
        auto f = search(x.data(), x.size(), {}, &ms);
        int ok = 0, fake = 0;
        for (size_t k = 0; k < c.params.size(); k++) for (auto& r : f) if (r.syncDl == c.params[k] / 4 && r.cellParam == c.params[k] && r.evenFirst >= 0) ok++;
        for (auto& r : f) { bool known = false; for (int p : c.params) if (r.syncDl == p / 4) known = true; fake += !known; }
        printf("  case %d: params", ci); for (int p : c.params) printf(" %d", p);
        printf(", %+.0f Hz, %+.0f ppm, SNR %.0f dB: %d of %zu, %.0f ms:", c.cfo, c.ppm, c.snr, ok, c.params.size(), ms);
        for (auto& r : f) printf(" [SYNC-DL %d param %d even %d S1 %d S2 %d mid %.1f cfo %+.0f snr %.1f]", r.syncDl, r.cellParam, r.evenFirst, r.quadS1, r.quadS2, r.midScore, r.cfoHz, r.snrDb);
        printf("\n");
        CHECK(ok == (int)c.params.size(), "case %d: %d of %zu", ci, ok, c.params.size());
        CHECK(fake == 0, "case %d: %d cells that are not there", ci, fake);
        for (auto& r : f) if (r.syncDl == c.params[0] / 4) { CHECK(std::fabs(r.cfoHz - c.cfo) < 500, "case %d cfo %.0f", ci, r.cfoHz); CHECK(c.s2 ? r.quadS2 > 0 && r.quadS1 > 0 : r.quadS2 == 0, "case %d quadruples", ci); }
        ci++;
    }
    // noise only
    { std::vector<cf32> z((size_t)(kFs * 0.08));
      std::mt19937 rng(4); std::normal_distribution<float> g(0.f, 0.1f); for (auto& v : z) v = cf32(g(rng), g(rng));
      auto f = search(z.data(), z.size()); CHECK(f.empty(), "noise: %zu cells", f.size()); }
    // the receiver thread and the mode row
    { std::vector<TxCell> tx(1); tx[0].cellParam = 50; tx[0].chipOffset = 77;
      auto s = generate(tx, (size_t)(kFs * 0.08), 12);
      TdscdmaReceiver rx; rx.configure(kFs); CHECK(rx.ready(), "ready");
      for (int r = 0; r < 3; r++) for (size_t o = 0; o < s.size(); o += 4096) rx.feed(s.data() + o, std::min<size_t>(4096, s.size() - o));
      for (int w = 0; w < 50 && rx.busy(); w++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      TdscdmaTelemetry t; rx.telemetry(t, 0);
      CHECK(t.cells.size() == 1 && t.cells[0].syncDl == 12 && t.cells[0].cellParam == 50, "receiver: %zu cells", t.cells.size());
      ModeTuning m = tdscdmaTuning(); CHECK(m.stdMode == 663 && m.minMhz < m.defMhz && m.defMhz < m.maxMhz, "tuning"); }
    return testSummary("tdscdma cell");
}
