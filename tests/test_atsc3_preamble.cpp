// ATSC 3.0, the front of a frame: bootstrap found in noise, then the Preamble (L1-Basic and L1-Detail across several OFDM symbols)
// decoded through a channel with an echo.
#include "dect2/atsc3_preamble.h"
#include <cmath>
#include <cstdio>
#include <random>

using namespace dect2;
using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static L1DetailPlp plp(int id) {
    L1DetailPlp p;
    p.id = id; p.start = 500 * id; p.size = 3000 + 7 * id; p.fecType = 3; p.mod = 4; p.cod = 5; p.tiMode = 0; p.fecBlockStart = id; p.type = 0;
    return p;
}

int main() {
    std::mt19937 rng(21);
    std::normal_distribution<float> g(0.f, 1.f);
    struct Case { int ps; int symbols; int mode; int plps; double snr; bool echo; } cases[] = {
        {0, 1, 2, 2, 25, false},    // 8K, one Preamble symbol, L1-Basic mode 1
        {0, 3, 3, 6, 25, true},     // three Preamble symbols
        {10, 2, 3, 4, 25, false},   // 8K GI 512 pilot spacing 6, L1-Basic mode 1
        {36, 2, 4, 5, 22, true},    // 16K, L1-Basic mode 2
        {95, 2, 3, 8, 25, false},   // 32K
    };
    for (auto& cs : cases) {
        Bootstrap bs;
        bs.minorVersion = 0; bs.numSymbols = 4; bs.systemBandwidth = 0; bs.bsrCoefficient = 8; bs.preambleStructure = cs.ps; bs.minTimeToNext = 7;
        L1Basic l1;
        l1.numSubframes = 0; l1.preambleNumSymbols = cs.symbols - 1; l1.preambleReducedCarriers = cs.symbols > 1 ? 2 : 0;
        l1.l1DetailFecType = cs.mode - 1;
        l1.firstSubFftSize = 0; l1.firstSubGuardInterval = 1; l1.firstSubNumOfdmSymbols = 99;
        L1Detail d;
        L1DetailSubframe sf;
        for (int i = 0; i < cs.plps; i++) sf.plps.push_back(plp(i));
        d.subframes = {sf};
        d.bsid = 0x1111;
        auto pre = buildPreamble(bs, l1, d);
        char m[120];
        snprintf(m, sizeof m, "preamble built (ps %d, %d symbols, mode %d)", cs.ps, cs.symbols, cs.mode);
        CHECK(!pre.empty(), m);
        if (pre.empty()) continue;

        // the bootstrap comes first, at its own sample rate; detect it in noise
        auto boot = generateBootstrap(bs);
        std::vector<cf32> bx(boot.size() + 30000);
        double sb = std::sqrt(std::pow(10.0, -cs.snr / 10.0) / 2.0);
        for (auto& v : bx) v = cf32(g(rng), g(rng)) * (float)sb;
        for (size_t i = 0; i < boot.size(); i++) bx[7000 + i] += boot[i];
        auto det = detectBootstrap(bx.data(), bx.size());
        CHECK(det.found && det.valid && det.info == bs && det.start == 7000, "bootstrap");

        // the Preamble follows at the frame's sample rate
        std::vector<cf32> rx(pre.size() + 500, cf32(0, 0));
        for (size_t i = 0; i < pre.size(); i++) rx[i] += pre[i];
        if (cs.echo) for (size_t i = 0; i + 25 < pre.size(); i++) rx[i + 25] += pre[i] * cf32(0.22f, -0.12f);
        double sp = std::sqrt(std::pow(10.0, -cs.snr / 10.0) / 2.0);
        for (auto& v : rx) v += cf32(g(rng), g(rng)) * (float)sp;
        auto res = decodePreamble(rx.data(), rx.size(), det.info);
        snprintf(m, sizeof m, "L1-Basic (ps %d)", cs.ps);
        CHECK(res.basicOk, m);
        snprintf(m, sizeof m, "L1-Detail (ps %d)", cs.ps);
        CHECK(res.detailOk, m);
        PreambleParams pq; preambleParams(cs.ps, pq);
        printf("  ps %3d  %d symbols  L1-Basic mode %d  L1-Detail mode %d: %d bytes, %d cells, basic %s detail %s\n", cs.ps, cs.symbols, pq.l1BasicMode, cs.mode,
               l1.l1DetailSizeBytes, l1.l1DetailTotalCells, res.basicOk ? "ok" : "FAILED", res.detailOk ? "ok" : "FAILED");
        if (res.detailOk) {
            CHECK(res.detail.subframes.size() == 1 && (int)res.detail.subframes[0].plps.size() == cs.plps && res.detail.bsid == 0x1111, "L1-Detail contents");
            CHECK(res.detail.subframes[0].plps[cs.plps - 1].size == 3000 + 7 * (cs.plps - 1), "PLP size field");
            CHECK(res.basic.l1DetailTotalCells == l1.l1DetailTotalCells && res.basic.preambleNumSymbols == cs.symbols - 1, "L1-Basic contents");
        }
    }
    printf(fails ? "atsc3 preamble: FAILED\n" : "atsc3 preamble: ok\n");
    return fails ? 1 : 0;
}
