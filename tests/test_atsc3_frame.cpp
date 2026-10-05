// A complete frame: Preamble with L1 and a subframe with two PLPs (different FEC and constellation), through a channel with an echo and noise,
// decoded back to the baseband packets. Frequency interleaving, boundary symbols with null cells, 8K, 16K and 32K FFT sizes.
#include "dect2/atsc3_frame.h"
#include "dect2/atsc3_bb.h"
#include <cmath>
#include <cstdio>
#include <random>

using namespace dect2;
using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main() {
    std::mt19937 rng(91);
    std::normal_distribution<float> g(0.f, 1.f);
    struct Case { int ps, fft, guard, sp, symbols, preSyms; int fec1, mod1, cod1, n1, fec2, mod2, cod2, n2; int nulls; double snr; } cases[] = {
        {0, 0, 1, 4, 30, 1, 0, 2, 6, 6, 2, 0, 3, 4, 20, 26},      // 8K: BCH 16K 64QAM 8/15, CRC 16K QPSK 5/15
        {30, 0, 3, 5, 24, 2, 1, 3, 9, 3, 4, 1, 5, 6, 0, 30},      // 8K GI 512 SP6_4, 64K 256QAM 11/15 and 16K 16QAM, two Preamble symbols
        {35, 1, 2, 4, 14, 1, 0, 2, 6, 4, 0, 0, 2, 4, 50, 26},     // 16K
        {95, 2, 1, 6, 8, 1, 0, 1, 5, 4, 2, 0, 3, 2, 0, 24},       // 32K
    };
    for (auto& cs : cases) {
        FrameSetup fs;
        fs.bs.preambleStructure = cs.ps; fs.bs.bsrCoefficient = 8; fs.bs.numSymbols = 4;
        fs.fftCode = cs.fft; fs.guardCode = cs.guard; fs.spPattern = cs.sp; fs.numSymbols = cs.symbols; fs.preambleSymbols = cs.preSyms;
        fs.sbsNullCells = cs.nulls; fs.l1DetailMode = 3;
        std::vector<FramePlp> plps(2);
        plps[0].id = 1; plps[0].fecType = cs.fec1; plps[0].mod = cs.mod1; plps[0].cod = cs.cod1;
        plps[1].id = 2; plps[1].fecType = cs.fec2; plps[1].mod = cs.mod2; plps[1].cod = cs.cod2;
        std::vector<std::vector<std::vector<uint8_t>>> sent(2);
        for (int k = 0; k < 2; k++) {
            Bicm b(plpBicm(plps[k]));
            CHECK(b.ok(), "PLP configuration");
            int count = k == 0 ? cs.n1 : cs.n2;
            for (int i = 0; i < count; i++) {
                std::vector<uint8_t> alp(b.kPayload() / 8);
                for (auto& v : alp) v = rng() & 255;
                auto pk = makeBbPacket(b.kPayload() / 8, alp, 0, i);
                plps[k].bbPackets.push_back(pk);
                sent[k].push_back(pk);
            }
        }
        L1Basic l1;
        int spare = 0;
        auto tx = buildFrame(fs, plps, &l1, &spare);
        char m[120];
        snprintf(m, sizeof m, "frame built (ps %d, FFT code %d)", cs.ps, cs.fft);
        CHECK(!tx.empty(), m);
        if (tx.empty()) continue;
        std::vector<cf32> rx(tx.size() + 300, cf32(0, 0));
        for (size_t i = 0; i < tx.size(); i++) rx[i] += tx[i];
        for (size_t i = 0; i + 25 < tx.size(); i++) rx[i + 25] += tx[i] * cf32(0.2f, -0.1f);
        double sig = 0;
        for (size_t i = 0; i < tx.size(); i++) sig += std::norm(rx[i]);
        sig /= tx.size();
        float sigma = (float)std::sqrt(sig / std::pow(10.0, cs.snr / 10.0) / 2.0);
        for (auto& v : rx) v += cf32(g(rng), g(rng)) * sigma;
        auto fr = decodeFrame(rx.data(), rx.size(), fs.bs);
        snprintf(m, sizeof m, "frame decoded (ps %d)", cs.ps);
        CHECK(fr.ok && fr.plps.size() == 2, m);
        if (!fr.ok) continue;
        int good = 0, total = 0;
        for (int k = 0; k < 2; k++) {
            total += (int)sent[k].size();
            CHECK(fr.plps[k].id == k + 1 && fr.plps[k].blocks == (int)sent[k].size(), "PLP blocks");
            for (size_t i = 0, j = 0; i < fr.plps[k].ok.size(); i++) {
                if (!fr.plps[k].ok[i]) continue;
                good += fr.plps[k].packets[j] == sent[k][i];
                j++;
            }
        }
        printf("  ps %3d  FFT %5d  %d Preamble symbols  %d data symbols: %d of %d baseband packets recovered, %d cells spare\n", cs.ps, fs.fftCode == 0 ? 8192 : fs.fftCode == 1 ? 16384 : 32768,
               cs.preSyms, cs.symbols, good, total, spare);
        snprintf(m, sizeof m, "all baseband packets (ps %d)", cs.ps);
        CHECK(good == total, m);
    }
    printf(fails ? "atsc3 frame: FAILED\n" : "atsc3 frame: ok\n");
    return fails ? 1 : 0;
}
