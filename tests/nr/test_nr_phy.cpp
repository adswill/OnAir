// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 5G NR cells physical layer: the polar code (construction against 38.212, an independent encoder, list decoding with puncturing,
// shortening and repetition), PSS / SSS / DMRS sequences, the PBCH payload and the whole PBCH chain, the sync raster.
#include "dect2/nr/nr_phy.h"
#include "dect2/common/stable_random.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <set>
using namespace dect2;
using namespace dect2::nr;
#include "../common/test_util.h"

static bool crc24c(const Bits& b, void*) { return cell::checkCrc(b, cell::kCrc24C, 24); }

// d = u G_N with G_N = F^(x)n built entry by entry: G[i][j] = 1 when (j & i) == j ... for F = [1 0; 1 1] the Kronecker power has
// G[i][j] = 1 exactly when the bits of j are a subset of the bits of i
static Bits refTransform(const Bits& u) {
    const size_t N = u.size();
    Bits d(N, 0);
    for (size_t i = 0; i < N; i++) if (u[i]) for (size_t j = 0; j < N; j++) if ((j & i) == j) d[j] ^= 1;
    return d;
}

static void polarRoundTrip(int A, int E, float snrDb, int trials, std::mt19937& rng, const char* what) {
    const PolarCode pc = polarCode(A + 24, E, 9, true);
    dect2::normal_distribution<float> nd(0, 1);
    const float sigma = std::pow(10.f, -snrDb / 20.f);
    int ok = 0;
    for (int t = 0; t < trials; t++) {
        Bits a((size_t)A);
        for (auto& v : a) v = rng() & 1;
        cell::appendCrc(a, cell::kCrc24C, 24);
        const Bits e = polarRateMatch(pc, polarEncode(pc, a));
        std::vector<float> llr((size_t)E);
        for (int i = 0; i < E; i++) { const float y = (1 - 2 * e[(size_t)i]) + sigma * nd(rng); llr[(size_t)i] = 2 * y / (sigma * sigma); }
        Bits c;
        if (polarDecode(pc, polarRateRecover(pc, llr.data()), 8, c, crc24c, nullptr) && c == a) ok++;
    }
    printf("  polar %s K=%d E=%d N=%d at %.1f dB: %d/%d\n", what, A + 24, E, pc.N, snrDb, ok, trials);
    CHECK(ok >= trials * 9 / 10, "polar %s round trip %d/%d", what, ok, trials);
}

int main() {
    std::mt19937 rng(3);
    // ---- polar construction: PBCH K=56 E=864 -> N=512, repetition, the 56 most reliable positions of Q
    {
        const PolarCode pc = polarCode(56, 864, 9, true);
        CHECK(pc.N == 512 && (int)pc.info.size() == 56, "PBCH N %d info %zu", pc.N, pc.info.size());
        // a few positions every 5G polar decoder agrees on: 511 is the most reliable, 0 the least; with K = 56 out of 512 the information
        // set holds only the high, heavy-weight indices
        std::set<int> I(pc.info.begin(), pc.info.end());
        CHECK(I.count(511) && I.count(510) && I.count(509) && I.count(507) && I.count(503) && I.count(495) && I.count(479) && I.count(447) && I.count(383) && I.count(255),
              "most reliable positions");
        CHECK(!I.count(0) && !I.count(1) && !I.count(256) && !I.count(128), "unreliable positions frozen");
        int minW = 99;
        for (int i : pc.info) { int w = 0; for (int b = 0; b < 9; b++) w += (i >> b) & 1; minW = std::min(minW, w); }
        CHECK(minW >= 5, "information set weight %d", minW);
        // the input interleaver for K = 56 (38.212 5.3.1.1): the first entries of the K_IL_max pattern that are >= 164 - 56 = 108
        CHECK(pc.pil[0] == 0 && pc.pil[1] == 2 && pc.pil[2] == 3 && pc.pil[3] == 5 && pc.pil[4] == 7 && pc.pil[5] == 10, "interleaver %d %d %d %d %d %d", pc.pil[0], pc.pil[1], pc.pil[2], pc.pil[3], pc.pil[4], pc.pil[5]);
        std::set<int> P(pc.pil.begin(), pc.pil.end());
        CHECK(P.size() == 56 && *P.begin() == 0 && *P.rbegin() == 55, "interleaver is a permutation");
        // the sub-block interleaver: J(n) for N = 512, 16 bits per sub-block, pattern 0 1 2 4 3 5 ...
        CHECK(pc.J[0] == 0 && pc.J[16] == 16 && pc.J[48] == 64 && pc.J[64] == 48 && pc.J[511] == 511, "sub-block interleaver");
        // the encoder against the direct Kronecker product
        for (int t = 0; t < 5; t++) {
            Bits c(56); for (auto& v : c) v = rng() & 1;
            const Bits d = polarEncode(pc, c);
            Bits u(512, 0);
            for (int k = 0; k < 56; k++) u[(size_t)pc.info[(size_t)k]] = c[(size_t)pc.pil[(size_t)k]];
            CHECK(d == refTransform(u), "encoder vs reference");
        }
    }
    // ---- sizes of the other DCI / UCI-like cases: mother code selection (5.3.1)
    {
        CHECK(polarCode(36, 108, 9).N == 128, "K36 E108 N %d", polarCode(36, 108, 9).N);    // DCI at aggregation level 1: punctured
        CHECK(polarCode(64, 432, 9).N == 512, "K64 E432 N %d", polarCode(64, 432, 9).N);    // aggregation level 4
        CHECK(polarCode(100, 140, 9).N == 256, "K100 E140 N %d", polarCode(100, 140, 9).N); // shortened
    }
    polarRoundTrip(32, 864, -1.0f, 40, rng, "PBCH (repetition)");
    polarRoundTrip(40, 216, 1.5f, 40, rng, "punctured");
    polarRoundTrip(80, 140, 5.0f, 40, rng, "shortened");
    // ---- sequences
    {
        float a[127], b[127];
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
            pss(i, a); pss(j, b);
            float s = 0; for (int n = 0; n < 127; n++) s += a[n] * b[n];
            CHECK(i == j ? s == 127 : std::fabs(s) < 20, "PSS %d x %d = %.0f", i, j, s);
        }
        pss(0, a);
        CHECK(a[0] == 1 && a[1] == -1 && a[2] == -1 && a[3] == 1 && a[4] == -1 && a[5] == -1 && a[6] == -1, "PSS 0 start (x = 0 1 1 0 1 1 1)");
        int worst = 0;
        for (int n1 = 0; n1 < 336; n1 += 7) for (int m1 = n1 + 1; m1 < 336; m1 += 11) {
            sss(n1, 1, a); sss(m1, 1, b);
            float s = 0; for (int n = 0; n < 127; n++) s += a[n] * b[n];
            worst = std::max(worst, (int)std::fabs(s));
        }
        CHECK(worst < 64, "SSS cross-correlation %d", worst);
        cf32 r0[144], r1[144];
        pbchDmrs(301, 0, r0); pbchDmrs(301, 5, r1);
        cf32 s(0, 0); for (int m = 0; m < 144; m++) s += r0[m] * std::conj(r1[m]);
        CHECK(std::abs(s) < 40 && std::fabs(std::abs(r0[7]) - 1) < 1e-5, "DMRS hypotheses %.1f", std::abs(s));
        CHECK(pbchDataRes(5).size() == 432 && pbchDmrsRes(5).size() == 144 && pbchDmrsRes(5)[0].k == 1 && pbchDmrsRes(5)[0].l == 1, "SSB grid");
    }
    // ---- MIB and the PBCH payload
    {
        Mib m; m.sfn = 789; m.scs30 = true; m.kSsb = 21; m.dmrsTypeAPos = 3; m.coreset0 = 12; m.ss0 = 5; m.barred = true; m.intraFreqResel = false; m.hrf = true;
        const Bits a = mibBits(m);
        CHECK(a.size() == 24 && a[0] == 0, "MIB 24 bits");
        const Bits c = pbchPayload(m, 77);
        CHECK(c.size() == 56 && crc24c(c, nullptr), "payload + CRC");
        Mib r;
        CHECK(pbchUnpack(c, 77, 8, r) && r.sfn == 789 && r.scs30 && r.kSsb == 21 && r.dmrsTypeAPos == 3 && r.coreset0 == 12 && r.ss0 == 5 && r.barred && !r.intraFreqResel && r.hrf,
              "payload round trip: sfn %d kssb %d", r.sfn, r.kSsb);
        // the scrambling depends on the 2nd / 3rd SFN LSB: the same MIB in another frame of the 80 ms gives other bits
        Mib m2 = m; m2.sfn = 789 ^ 2;
        CHECK(pbchPayload(m2, 77) != c, "scrambled per frame");
        // 7.1.2: the half frame bit sits at G(10) = 0 unscrambled, SFN bit 2 at G(7) = 6 and bit 1 at G(8) = 24
        CHECK(c[0] == 1 && c[6] == ((789 >> 2) & 1) && c[24] == ((789 >> 1) & 1), "unscrambled timing bits");
    }
    // ---- the whole PBCH chain, every SSB index, Lmax 8 and 4, with noise
    {
        dect2::normal_distribution<float> nd(0, 1);
        int ok = 0, n = 0;
        for (int lmax : {8, 4})
            for (int i = 0; i < lmax; i++)
                for (int hrf = 0; hrf < 2; hrf++) {
                    Mib m; m.sfn = (37 * i + 11 * hrf + lmax) & 1023; m.kSsb = 9 + i; m.coreset0 = i; m.ss0 = 15 - i; m.hrf = hrf; m.ssbIdx = i; m.lmax = lmax;
                    const int pci = 1007 - 13 * i;
                    const Bits e = pbchEncode(m, pci);
                    std::vector<float> llr(864);
                    for (int k = 0; k < 864; k++) llr[(size_t)k] = (1 - 2 * e[(size_t)k]) + 0.7f * nd(rng);
                    const int issb = lmax == 4 ? i + 4 * hrf : i;
                    Mib r;
                    n++;
                    if (pbchDecode(llr.data(), pci, issb, r) && r.sfn == m.sfn && r.ssbIdx == i && r.hrf == (hrf != 0) && r.kSsb == m.kSsb && r.coreset0 == i) ok++;
                    else printf("  PBCH lmax %d ssb %d hrf %d failed\n", lmax, i, hrf);
                }
        CHECK(ok == n, "PBCH chain %d/%d", ok, n);
        // a wrong cell id never passes the CRC
        Mib m; m.sfn = 5;
        const Bits e = pbchEncode(m, 100);
        std::vector<float> llr(864);
        for (int k = 0; k < 864; k++) llr[(size_t)k] = 1 - 2 * e[(size_t)k];
        Mib r;
        CHECK(!pbchDecode(llr.data(), 101, 0, r), "wrong PCI decoded");
    }
    // ---- the sync raster (38.104 5.4.3)
    {
        CHECK(std::fabs(gscnToHz(7875) - 3541.44e6) < 1 && hzToGscn(3541.44e6) == 7875, "GSCN 7875");
        CHECK(std::fabs(gscnToHz(5279) - 2112.05e6) < 1 && std::fabs(gscnToHz(1982) - 793.25e6) < 1 && std::fabs(gscnToHz(6246) - 2498.55e6) < 1, "GSCN below 3 GHz");
        CHECK(hzToGscn(793.25e6) == 1982 && hzToGscn(2498.55e6) == 6246, "Hz to GSCN");
        CHECK(nrBandOfGscn(7875, 30) == 78 && nrBandOfGscn(8200, 30) == 77 && nrBandOfGscn(1990, 15) == 28 && nrBandOfGscn(2030, 15) == 20 && nrBandOfGscn(7875, 15) == 0, "bands");
        CHECK(nrBandOfGscn(6253, 30) == 0 && nrBandOfGscn(6255, 30) == 41, "n41 step 3");
        bool ok = true;
        for (const auto& b : nrBands()) {
            if ((b.gscnHi - b.gscnLo) % b.gscnStep || gscnToHz(b.gscnLo) < b.dlLowMhz * 1e6 || gscnToHz(b.gscnHi) > b.dlHighMhz * 1e6) ok = false;
            for (int g = b.gscnLo; g <= b.gscnHi; g += b.gscnStep) if (hzToGscn(gscnToHz(g)) != g || !nrBandOfGscn(g, b.scsKhz)) ok = false;
        }
        CHECK(ok && nrBands().size() >= 25, "NR band table consistent");
        CHECK(nrBandOfGscn(8496, 30) == 79 && nrBandOfGscn(8490, 30) == 0 && nrBandOfGscn(2200, 15) == 5 && nrBandOfGscn(1600, 15) == 71 && nrBandOfGscn(7900, 30) == 78, "new NR bands");
        CHECK(std::fabs(gscnToHz(8480) - 4412.64e6) < 1 && std::fabs(gscnToHz(2177) - 871.25e6) < 1, "n79 / n5 raster");
        int rb, sy, of;
        CHECK(coreset0Info(30, 30, 12, rb, sy, of) && rb == 48 && sy == 1 && of == 16, "CORESET#0 13-4");
        CHECK(coreset0Info(15, 15, 3, rb, sy, of) && rb == 24 && sy == 3 && of == 0 && !coreset0Info(15, 15, 15, rb, sy, of), "CORESET#0 13-1");
        CHECK(ssbFirstSymbol(0, 8) == 2 && ssbFirstSymbol(1, 8) == 8 && ssbFirstSymbol(7, 8) == 50 && ssbFirstSymbol(4, 4) == -1, "SSB symbols");
        Numerology a; a.scsKhz = 30;
        CHECK(a.halfLen() == 38400 && a.symStart(14) == 3840 && a.cpLen(0) == 22 && a.cpLen(1) == 18 && a.symStart(140) == 38400, "numerology 30 kHz");
        Numerology b; b.scsKhz = 15;
        CHECK(b.halfLen() == 19200 && b.symStart(7) == 1920 && b.cpLen(7) == 20, "numerology 15 kHz");
    }
    printf(fails ? "nr phy: %d FAILED\n" : "nr phy: all passed\n", fails);
    return fails ? 1 : 0;
}
