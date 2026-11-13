// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell towers, LTE building blocks: CRC, the tail-biting convolutional code, the turbo code and its rate matching, the QPP interleaver,
// DCI formats 1A / 1C, the control region mapping, and the SIB1 - SIB5 UPER coder (round trips and fixed points from the specifications).
#include "dect2/cell/cell_asn.h"
#include "dect2/common/stable_random.h"
#include "dect2/cell/cell_fec.h"
#include "dect2/cell/cell_lte.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <set>
using namespace dect2::cell;
#include "../common/test_util.h"

static Bits randBits(std::mt19937& r, size_t n) { Bits b(n); for (auto& v : b) v = r() & 1; return b; }

int main() {
    std::mt19937 rng(5);
    dect2::normal_distribution<float> nd(0.f, 1.f);
    // CRC: the parity of the message followed by its parity is zero; a flipped bit is caught
    {
        Bits b = randBits(rng, 100);
        appendCrc(b, kCrc24A, 24);
        CHECK(checkCrc(b, kCrc24A, 24), "crc24a");
        b[17] ^= 1;
        CHECK(!checkCrc(b, kCrc24A, 24), "crc24a flip");
        Bits m = randBits(rng, 24);
        appendCrc(m, kCrc16, 16, 0xFFFF);
        CHECK(checkCrc(m, kCrc16, 16, 0xFFFF) && !checkCrc(m, kCrc16, 16, 0), "crc16 mask");
    }
    // Gold sequence: c(n) for c_init = 0 is x1 alone after 1600 shifts; it must not be all zeros and must differ per c_init
    {
        const Bits a = gold(0, 64), b = gold(1, 64);
        int ones = 0; for (auto v : a) ones += v;
        CHECK(ones > 10 && a != b, "gold");
    }
    // convolutional code: noisy round trip at Es/N0 0 dB for K = 40 (MIB) and 43 (DCI)
    for (int K : {40, 43, 57}) {
        int bad = 0;
        for (int t = 0; t < 50; t++) {
            const Bits in = randBits(rng, (size_t)K);
            const Bits e = convRateMatch(convEncode(in), 576);
            std::vector<float> l(e.size());
            for (size_t i = 0; i < e.size(); i++) l[i] = (e[i] ? -1.f : 1.f) * 2 + nd(rng) * 1.4f;
            const std::vector<float> d = convRateRecover(l.data(), l.size(), (size_t)K);
            if (viterbiTb(d.data(), (size_t)K) != in) bad++;
        }
        CHECK(bad <= 1, "viterbi K=%d: %d of 50 wrong", K, bad);
    }
    // QPP: every size is a permutation
    {
        int badK = 0;
        for (int B = 40; B <= 6144; B += 8) {
            const int K = turboSizeFor(B);
            if (K != B) continue;
            int f1, f2; qppParams(K, f1, f2);
            std::vector<char> s((size_t)K, 0);
            for (int i = 0; i < K; i++) s[(size_t)(((int64_t)f1 * i + (int64_t)f2 * i % K * i) % K)] = 1;
            for (char v : s) if (!v) { badK++; break; }
        }
        CHECK(badK == 0, "%d QPP sizes are not permutations", badK);
        CHECK(turboSizeFor(41) == 48 && turboSizeFor(6144) == 6144 && turboSizeFor(6145) == 0, "turbo sizes");
    }
    // turbo code: filler bits, every redundancy version, a code rate near 1/2 at Es/N0 2 dB
    for (int B : {64, 300, 1000}) {
        const int K = turboSizeFor(B), F = K - B;
        for (int rv = 0; rv < 4; rv++) {
            Bits cb((size_t)F, 0);
            const Bits in = randBits(rng, (size_t)B);
            cb.insert(cb.end(), in.begin(), in.end());
            const size_t E = (size_t)(B * 2 + 40);
            const Bits e = turboRateMatch(turboEncode(cb), K, F, E, rv);
            std::vector<float> l(E);
            for (size_t i = 0; i < E; i++) l[i] = (e[i] ? -1.f : 1.f) * 1.6f + nd(rng);
            std::vector<float> d;
            turboRateRecover(l.data(), E, K, F, rv, d);
            Bits out;
            turboDecode(d, K, F, 8, out);
            CHECK(Bits(out.begin() + F, out.end()) == in, "turbo B=%d rv=%d", B, rv);
        }
    }
    // DCI 1A and 1C sizes (36.212 5.3.3.1.3 / 5.3.3.1.4, FDD): 1.4 / 3 / 5 / 10 / 15 / 20 MHz
    {
        const int n[6] = {6, 15, 25, 50, 75, 100}, a1[6] = {21, 22, 25, 27, 27, 28}, c1[6] = {8, 10, 12, 13, 14, 15};
        for (int i = 0; i < 6; i++) {
            CHECK(lteDci1ASize(n[i]) == a1[i], "1A size %d RB: %d", n[i], lteDci1ASize(n[i]));
            CHECK(lteDci1CSize(n[i]) == c1[i], "1C size %d RB: %d", n[i], lteDci1CSize(n[i]));
        }
        LteDciSi d; d.format = 0; d.rbStart = 3; d.lCrb = 4; d.mcs = 5; d.rv = 2; d.nPrb1A = 3;
        LteDciSi r;
        CHECK(lteDciParse(lteDciBuild(d, 50), 50, 0, r) && r.rbStart == 3 && r.lCrb == 4 && r.mcs == 5 && r.rv == 2 && r.nPrb1A == 3 && r.tbs == lteTbs(5, 3), "1A round trip");
        LteDciSi c; c.format = 1; c.distributed = true; c.rbStart = 8; c.lCrb = 8; c.mcs = 7;
        CHECK(lteDciParse(lteDciBuild(c, 50), 50, 1, r) && r.rbStart == 8 && r.lCrb == 8 && r.tbs == lteTbs1C(7), "1C round trip");
        // distributed VRBs map to distinct PRBs inside the carrier, in both slots
        for (int nrb : {6, 25, 50, 100}) {
            LteDciSi x; x.format = 0; x.distributed = true; x.rbStart = 0; x.lCrb = 4;
            std::vector<int> s0, s1;
            ltePrbs(x, nrb, s0, s1);
            std::set<int> u0(s0.begin(), s0.end()), u1(s1.begin(), s1.end());
            bool in = true; for (int v : s0) if (v < 0 || v >= nrb) in = false; for (int v : s1) if (v < 0 || v >= nrb) in = false;
            CHECK(u0.size() == 4 && u1.size() == 4 && in, "distributed PRBs %d RB", nrb);
        }
        CHECK(lteTbs(0, 2) == 32 && lteTbs(26, 3) == 2216 && lteTbs1C(31) == 1736, "TBS tables");
    }
    // the control region: PCFICH, PHICH and the PDCCH quadruplets use every REG of the control symbols exactly once
    for (int nrb : {6, 25, 50, 100})
        for (int ports : {1, 2, 4})
            for (int cfi = 1; cfi <= 3; cfi++) {
                LteCellCfg c; c.nRb = nrb; c.ports = ports; c.pci = 301; c.phichRes = 1;
                const LteControl ctl = lteControl(c, cfi, 0);
                std::set<std::pair<int, int>> used;
                bool dup = false;
                auto add = [&](const LteReg& r) { if (!used.insert({r.l, r.k0}).second) dup = true; };
                for (auto& r : ctl.pcfich) add(r);
                for (auto& r : ctl.phich) add(r);
                for (auto& r : ctl.pdcchQuad) add(r);
                int total = 0;
                for (int l = 0; l < lteCtrlSyms(cfi, nrb); l++) {
                    const bool rs = l == 0 || (l == 1 && ports == 4);
                    total += rs ? 2 * nrb : 3 * nrb;
                }
                CHECK(!dup && (int)used.size() == total && ctl.pcfich.size() == 4, "control %d RB %d ports cfi %d: %zu of %d", nrb, ports, cfi, used.size(), total);
            }
    // SIB1 - SIB5 UPER round trip
    {
        LteSib1 s;
        Plmn p; p.mcc = 246; p.mnc = 1; s.plmns.push_back(p);
        Plmn q; q.mcc = 246; q.mnc = 123; q.mncDigits = 3; s.plmns.push_back(q);
        s.tac = 0x2F3A; s.cellId = (123456u << 8) | 21u; s.band = 20; s.qRxLevMinDbm = -124; s.pMax = 23;
        s.sched = {{16, {}}, {32, {3, 13}}, {64, {4, 5}}};
        s.siWindowMs = 20; s.valueTag = 9;
        LteBcch b;
        CHECK(lteParseBcch(lteEncodeSib1(s), b) && b.isSib1, "sib1 parse");
        const LteSib1& r = b.sib1;
        CHECK(r.plmns.size() == 2 && r.plmns[0].mcc == 246 && r.plmns[0].mnc == 1 && r.plmns[1].mnc == 123 && r.plmns[1].mncDigits == 3, "plmn");
        CHECK(r.tac == s.tac && r.cellId == s.cellId && r.band == 20 && r.pMax == 23 && r.qRxLevMinDbm == -124, "sib1 fields");
        CHECK(r.sched.size() == 3 && r.sched[1].sibs.size() == 2 && r.sched[1].sibs[1] == 13 && r.sched[2].periodRf == 64 && r.siWindowMs == 20 && r.valueTag == 9, "sib1 schedule");
        CHECK(plmnText(r.plmns[0]) == "246-01" && plmnText(r.plmns[1]) == "246-123", "plmn text");
        LteSib2 s2; s2.acBarring = true; s2.acEmergencyBarred = true; s2.acDataBarred = true; s2.numRaPreambles = 52; s2.refSignalPowerDbm = 18; s2.pb = 1;
        s2.ulEarfcn = 24300; s2.ulBwRb = 50; s2.timeAlignMs = -1; s2.t300 = 1000; s2.n310 = 10; s2.rootSeqIndex = 700; s2.n1PucchAn = 2000;
        LteSib3 s3; s3.qHystDb = 4; s3.priority = 5; s3.sIntraSearch = 62; s3.sNonIntraSearch = 8; s3.threshServingLow = 6; s3.tReselSec = 2;
        LteSib4 s4; s4.neigh = {{12, 0}, {400, -4}}; s4.black = {{20, 4}};
        LteSib5 s5; LteSib5Freq f; f.earfcn = 1300; f.bwRb = 75; f.priority = 6; f.qOffsetFreqDb = 2; f.neigh = {{88, 0}}; s5.freqs = {f};
        LteBcch c;
        CHECK(lteParseBcch(lteEncodeSi(&s2, &s3, &s4, &s5), c) && !c.isSib1 && c.has2 && c.has3 && c.has4 && c.has5, "si parse");
        CHECK(c.sib2.acBarring && c.sib2.acEmergencyBarred && c.sib2.acDataBarred && !c.sib2.acSignallingBarred, "sib2 barring");
        CHECK(c.sib2.numRaPreambles == 52 && c.sib2.refSignalPowerDbm == 18 && c.sib2.pb == 1 && c.sib2.ulEarfcn == 24300 && c.sib2.ulBwRb == 50 && c.sib2.timeAlignMs == -1, "sib2");
        CHECK(c.sib2.rootSeqIndex == 700 && c.sib2.n1PucchAn == 2000 && c.sib2.t300 == 1000 && c.sib2.n310 == 10, "sib2 more");
        CHECK(c.sib3.qHystDb == 4 && c.sib3.priority == 5 && c.sib3.sIntraSearch == 62 && c.sib3.sNonIntraSearch == 8 && c.sib3.tReselSec == 2, "sib3");
        CHECK(c.sib4.neigh.size() == 2 && c.sib4.neigh[1].pci == 400 && c.sib4.neigh[1].qOffsetDb == -4 && c.sib4.black.size() == 1 && c.sib4.black[0].second == 4, "sib4");
        CHECK(c.sib5.freqs.size() == 1 && c.sib5.freqs[0].earfcn == 1300 && c.sib5.freqs[0].bwRb == 75 && c.sib5.freqs[0].qOffsetFreqDb == 2 && c.sib5.freqs[0].neigh[0].pci == 88, "sib5");
        // a cut message must not parse as a full one
        Bits cut = lteEncodeSib1(s); cut.resize(30);
        LteBcch d;
        CHECK(!lteParseBcch(cut, d), "cut sib1");
    }
    printf(fails ? "cell phy: %d FAILED\n" : "cell phy: all passed\n", fails);
    return fails ? 1 : 0;
}
