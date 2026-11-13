// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell towers, LTE test signal: one eNodeB carrier with its broadcast channels, built from the same specification tables the receiver
// reads (36.211 / 36.212). Nothing is transmitted: the samples only exist in memory.
#include "cell_lte_int.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace dect2::cell {

namespace {

struct Gen {
    const LteGenCfg& g;
    LteCellCfg c;
    int N, nsym, nsc;
    std::mt19937 rng;
    std::vector<LteGrid> port;
    explicit Gen(const LteGenCfg& gc) : g(gc), c(gc.cell), rng(gc.seed) {
        N = lteSymbPerSlot(c.extCp); nsym = 2 * N; nsc = 12 * c.nRb;
    }

    void put(int p, int l, int k, cf32 v) { port[(size_t)p].at(l, k) = v; }

    // symbols mapped with transmit diversity onto REs (in order)
    void mapTxd(const std::vector<cf32>& d, const std::vector<std::pair<int, int>>& res) {
        std::vector<std::vector<cf32>> y;
        lteTxdEncode(d, c.ports, y);
        for (int p = 0; p < c.ports; p++) for (size_t i = 0; i < res.size() && i < y[(size_t)p].size(); i++) put(p, res[i].first, res[i].second, y[(size_t)p][i]);
    }

    std::vector<cf32> modBits(const Bits& b) {
        std::vector<cf32> d;
        for (size_t i = 0; i + 1 < b.size(); i += 2) d.push_back(qpsk(b[i], b[i + 1]));
        return d;
    }

    Bits pbchBlock(int sfn) {
        Bits mib;
        auto put = [&](int v, int n) { for (int i = n - 1; i >= 0; i--) mib.push_back((v >> i) & 1); };
        put(lteRbToBwIndex(c.nRb), 3); put(c.phichDur, 1); put(c.phichRes, 2); put((sfn >> 2) & 0xff, 8); put(c.mibTail & 0x3ff, 10);
        const uint32_t mask = c.ports == 1 ? 0 : c.ports == 2 ? 0xFFFF : 0x5555;
        appendCrc(mib, kCrc16, 16, mask);
        const int M = ltePbchBits(c.extCp);
        Bits e = convRateMatch(convEncode(mib), (size_t)M);
        const Bits cs = gold((uint32_t)c.pci, (size_t)M);
        for (int i = 0; i < M; i++) e[(size_t)i] ^= cs[(size_t)i];
        return e;
    }

    struct Dci { LteDciSi d; uint16_t rnti; Bits tb; int cce, L; };

    void subframe(int sfn, int sf, std::vector<Dci> dcis) {
        port.assign((size_t)c.ports, LteGrid());
        for (auto& p : port) p.init(nsym, nsc);
        const int vs = c.pci % 6;
        (void)vs;
        // synchronisation (port 0)
        cf32 pss[62]; float sss[62];
        ltePss(c.pci % 3, pss);
        const int kc = 6 * c.nRb - 31;
        auto sync = [&](int lp, int ls, int which) {
            lteSss(c.pci / 3, c.pci % 3, which, sss);
            for (int n = 0; n < 62; n++) { put(0, lp, kc + n, pss[n]); put(0, ls, kc + n, cf32(sss[n], 0)); }
        };
        if (!c.tdd) { if (sf == 0 || sf == 5) sync(N - 1, N - 2, sf); }
        else {
            if (sf == 0 || sf == 5) { lteSss(c.pci / 3, c.pci % 3, sf, sss); for (int n = 0; n < 62; n++) put(0, 2 * N - 1, kc + n, cf32(sss[n], 0)); }
            if (sf == 1 || sf == 6) for (int n = 0; n < 62; n++) put(0, 2, kc + n, pss[n]);
        }
        // reference signals
        cf32 r[2 * kMaxRb];
        for (int p = 0; p < c.ports; p++) for (int lsf = 0; lsf < nsym; lsf++) {
            const int l = lsf % N, ns = 2 * sf + lsf / N;
            const bool has = (p <= 1) ? (l == 0 || l == N - 3) : (l == 1);
            if (!has) continue;
            lteCrsSeq(c.pci, ns, l, c.extCp, r);
            for (int k = 0; k < nsc; k++) if (lteCrsAt(c, sf, lsf, k, p)) put(p, lsf, k, r[k / 6 + kMaxRb - c.nRb]);
        }
        // PBCH
        if (sf == 0) {
            const Bits blk = pbchBlock(sfn);
            const int M = ltePbchBits(c.extCp), per = M / 4, seg = sfn % 4;
            Bits b(blk.begin() + seg * per, blk.begin() + (seg + 1) * per);
            std::vector<std::pair<int, int>> res;
            for (int l = N; l < N + 4; l++) for (int kk = 0; kk < 72; kk++) {
                const int k = 6 * c.nRb - 36 + kk;
                const bool rs = (l - N <= 1) || (c.extCp && l - N == 3);
                if (rs && (k % 3) == (c.pci % 6) % 3) continue;
                res.push_back({l, k});
            }
            mapTxd(modBits(b), res);
        }
        const LteControl ctl = lteControl(c, g.cfi, sf);
        // PCFICH
        {
            Bits b(kCfiCode[g.cfi - 1], kCfiCode[g.cfi - 1] + 32);
            const Bits cs = gold((uint32_t)((sf + 1) * (2 * c.pci + 1) * 512 + c.pci), 32);
            for (int i = 0; i < 32; i++) b[(size_t)i] ^= cs[(size_t)i];
            std::vector<std::pair<int, int>> res;
            for (const auto& rg : ctl.pcfich) for (int i = 0; i < 4; i++) res.push_back({rg.l, rg.k[i]});
            mapTxd(modBits(b), res);
        }
        // PDCCH
        const int M = (int)ctl.pdcchQuad.size();
        std::vector<int8_t> bits((size_t)M * 8, -1);   // -1 = <NIL>
        for (const auto& x : dcis) {
            Bits p = lteDciBuild(x.d, c.nRb);
            appendCrc(p, kCrc16, 16, x.rnti);
            const Bits e = convRateMatch(convEncode(p), (size_t)(72 * x.L));
            for (int i = 0; i < 72 * x.L; i++) bits[(size_t)(72 * x.cce + i)] = (int8_t)e[(size_t)i];
        }
        {
            const Bits cs = gold((uint32_t)(sf * 512 + c.pci), bits.size());
            std::vector<cf32> d(bits.size() / 2);
            std::vector<bool> nil(d.size());
            for (size_t i = 0; i < d.size(); i++) {
                if (bits[2 * i] < 0) { nil[i] = true; continue; }
                d[i] = qpsk((uint8_t)(bits[2 * i] ^ cs[2 * i]), (uint8_t)(bits[2 * i + 1] ^ cs[2 * i + 1]));
            }
            // per quadruplet: transmit diversity inside the quadruplet
            for (int q = 0; q < M; q++) {
                const LteReg& rg = ctl.pdcchQuad[(size_t)q];
                if (nil[(size_t)(4 * q)]) continue;
                std::vector<cf32> dq(d.begin() + 4 * q, d.begin() + 4 * q + 4);
                std::vector<std::pair<int, int>> res;
                for (int i = 0; i < 4; i++) res.push_back({rg.l, rg.k[i]});
                mapTxd(dq, res);
            }
        }
        // PDSCH
        std::vector<bool> usedPrb((size_t)c.nRb, false);
        for (const auto& x : dcis) {
            std::vector<int> p0, p1;
            ltePrbs(x.d, c.nRb, p0, p1);
            for (int v : p0) usedPrb[(size_t)v] = true;
            for (int v : p1) usedPrb[(size_t)v] = true;
            const auto res = ltePdschRes(c, sf, g.cfi, p0, p1);
            Bits tb = x.tb;
            appendCrc(tb, kCrc24A, 24);
            const int B = (int)tb.size(), K = turboSizeFor(B), F = K - B;
            Bits cb((size_t)F, 0); cb.insert(cb.end(), tb.begin(), tb.end());
            const size_t G = res.size() * 2;
            const int NL = c.ports > 1 ? 2 : 1;
            const size_t E = (size_t)(NL * 2) * (G / (size_t)(NL * 2));
            Bits e = turboRateMatch(turboEncode(cb), K, F, E, x.d.rv);
            const Bits cs = gold(((uint32_t)x.rnti << 14) + (uint32_t)(sf << 9) + (uint32_t)c.pci, e.size());
            for (size_t i = 0; i < e.size(); i++) e[i] ^= cs[i];
            mapTxd(modBits(e), res);
        }
        // other users' data in the free resource blocks
        if (g.loadPdsch) {
            std::vector<int> prb;
            for (int v = 0; v < c.nRb; v++) if (!usedPrb[(size_t)v]) prb.push_back(v);
            const auto res = ltePdschRes(c, sf, g.cfi, prb, prb);
            std::vector<cf32> d(res.size());
            for (auto& v : d) v = qpsk(rng() & 1, rng() & 1);
            mapTxd(d, res);
        }
    }
};

void applyTaps(std::vector<cf32>& x, const std::array<float, 4>& t, double fs) {
    const double sc = fs / 30.72e6;
    const int d1 = (int)std::lround(t[0] * sc), d2 = (int)std::lround(t[2] * sc);
    const float g1 = std::pow(10.f, t[1] / 20), g2 = std::pow(10.f, t[3] / 20);
    std::vector<cf32> y(x.size(), cf32(0, 0));
    for (size_t n = 0; n < x.size(); n++) {
        cf32 v = x[n];
        if (g1 > 1e-4f && n >= (size_t)d1) v += g1 * std::polar(1.f, 0.7f) * x[n - (size_t)d1];
        if (g2 > 1e-4f && n >= (size_t)d2) v += g2 * std::polar(1.f, 2.1f) * x[n - (size_t)d2];
        y[n] = v;
    }
    x.swap(y);
}

} // namespace

std::vector<cf32> lteGenerate(const LteGenCfg& gc, int nFrames) {
    Gen G(gc);
    const LteCellCfg& c = G.c;
    const int nfft = lteNfft(c.nRb);
    std::vector<std::vector<cf32>> tx((size_t)c.ports);
    for (int f = 0; f < nFrames; f++) {
        const int sfn = (gc.sfn0 + f) & 1023;
        for (int sf = 0; sf < 10; sf++) {
            std::vector<Gen::Dci> dcis;
            int cce = 0;
            for (const auto& s : gc.sibs) {
                if (s.subframe != sf || (sfn % s.period) != s.sfnMod) continue;
                // size the allocation: TBS from the 1A table (columns 2 and 3), a code rate around 1/3
                const int need = (int)s.bits.size();
                Gen::Dci d;
                d.rnti = kSiRnti;
                d.d.format = s.c1 ? 1 : 0;
                int best = -1, col = 2;
                if (!s.c1) { for (int i = 0; i < 27 && best < 0; i++) for (int cc = 2; cc <= 3; cc++) if (lteTbs(i, cc) >= need) { best = i; col = cc; break; } }
                else for (int i = 0; i < 32 && best < 0; i++) if (lteTbs1C(i) >= need) best = i;
                if (best < 0) continue;
                d.d.mcs = best; d.d.nPrb1A = col;
                d.d.tbs = s.c1 ? lteTbs1C(best) : lteTbs(best, col);
                const int perPrb = (c.ports == 4 ? 104 : c.ports == 2 ? 116 : 128) * 2;
                int L = std::max(2, (d.d.tbs + 24) * 3 / perPrb + 1);
                if (s.c1) { const int st = c.nRb < 50 ? 2 : 4; L = (L + st - 1) / st * st; }
                L = std::min(L, std::min(c.nRb, 16));
                d.d.lCrb = L;
                d.d.distributed = s.c1 || (sf != 5);   // SIB1 localized, the other messages distributed
                d.d.rbStart = d.d.distributed ? 0 : std::max(0, c.nRb - L - 1);
                if (sf == 5 && sfn % 2 == 0 && !s.c1) { const int k = (sfn / 2) % 4; d.d.rv = ((3 * k + 1) / 2) % 4; }
                d.tb = s.bits; d.tb.resize((size_t)d.d.tbs, 0);
                d.L = 4; d.cce = cce; cce += 4;
                dcis.push_back(d);
            }
            if (gc.pagingDci && sf == 9 && cce + 4 <= 16) {
                Gen::Dci d;
                d.rnti = 0xFFFE;   // P-RNTI: the receiver must not decode it
                d.d.format = 0; d.d.mcs = 2; d.d.nPrb1A = 2; d.d.tbs = lteTbs(2, 2); d.d.lCrb = 3; d.d.rbStart = 1;
                d.tb.assign((size_t)d.d.tbs, 0);
                for (auto& b : d.tb) b = G.rng() & 1;
                d.L = 4; d.cce = cce; cce += 4;
                dcis.push_back(d);
            }
            // keep the CCEs inside the control region
            const LteControl ctl = lteControl(c, gc.cfi, sf);
            dcis.erase(std::remove_if(dcis.begin(), dcis.end(), [&](const Gen::Dci& d) { return d.cce + d.L > ctl.nCce; }), dcis.end());
            G.subframe(sfn, sf, dcis);
            for (int p = 0; p < c.ports; p++) lteOfdmMod(G.port[(size_t)p], c.nRb, c.extCp, nfft, tx[(size_t)p]);
        }
    }
    std::vector<cf32> out(tx[0].size(), cf32(0, 0));
    for (int p = 0; p < c.ports; p++) {
        if ((size_t)p < gc.portTaps.size()) applyTaps(tx[(size_t)p], gc.portTaps[(size_t)p], lteRate(nfft));
        for (size_t i = 0; i < out.size(); i++) out[i] += tx[(size_t)p][i];
    }
    double e = 0;
    for (auto& v : out) e += std::norm(v);
    const float sc = (float)(0.2 / std::sqrt(e / std::max<size_t>(1, out.size()) + 1e-20));
    for (auto& v : out) v *= sc;
    return out;
}

} // namespace dect2::cell
