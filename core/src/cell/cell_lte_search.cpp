// Cell towers, LTE: the cell search over one block of samples. PSS candidates are tracked, the SSS names the cell, the PBCH gives
// its MIB and the system information is read on SI-RNTI. Decoded cells are cancelled from under weaker ones, in several passes.
// The state of one search lives in LteSearch; lteSearchDecode() at the end is the public entry point.
#include "cell_lte_sync.h"
#include "dect2/cell/cell_dsp.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace dect2::cell {
using namespace lte_rx;

namespace {

static const bool dbg = getenv("CELL_DEBUG") != nullptr;
static constexpr int portsOf[3] = {1, 2, 4};

// One search over one block: the block (x), its copy at the sync rate (y) and everything found so far.
struct LteSearch {
    const cf32* x; size_t n; double fs, offsetHz; const LteSearchOpts& o;
    double toSync;
    std::vector<cf32> y;                 // the block at the sync rate, shifted to the search offset
    std::vector<Cand> cands;             // PSS candidates
    std::vector<LteCellResult> out;      // the cells listed
    // cells found so far: cancelled from the SSS search of the others; a second pass looks behind every PSS timing again with all of them out
    std::vector<KnownSync> known;
    std::vector<Decoded> decoded;        // cells whose MIB decoded
    const double frameLenSync = 2.0 * kHalf;
    // cells whose PBCH did not decode: tried again at the end with the set-up of a decoded cell at their timing
    struct Pending { LteCellResult R; double fs0; Track tr; int outIdx; bool done = false; };
    std::vector<Pending> pending;

    LteSearch(const cf32* x_, size_t n_, double fs_, double offsetHz_, const LteSearchOpts& o_, double toSync_, std::vector<cf32> y_)
        : x(x_), n(n_), fs(fs_), offsetHz(offsetHz_), o(o_), toSync(toSync_), y(std::move(y_)) { decoded.reserve(64); }

    static bool hasSib1(const LteCellResult& r) { for (const auto& m : r.si) if (m.subframe == 5) return true; return false; }

    void remember(const SssResult& ss, int nid2, const Track& t) {
        KnownSync k; k.nid1 = ss.nid1; k.nid2 = nid2; k.parity = ss.sf0Parity; k.off = !ss.tdd ? (ss.ext ? 128 + 32 : 128 + 9) : (ss.ext ? 480 : 412); k.t = t.t;
        known.push_back(std::move(k));
    }

    // decoded cells at (about) the same frame timing and carrier: a synchronised network's sectors. k: frame index offset (theirs = ours + k)
    void sameTiming(double fs0, double cfo, bool ext, int pci, std::vector<std::pair<const Decoded*, int>>& res) {
        res.clear();
        for (const Decoded& d : decoded) {
            if (d.pci == pci || d.ext != ext || std::fabs(d.cfo - cfo) > 1500) continue;
            const double dt = fs0 - d.fs0;
            const int k = (int)std::lround(dt / frameLenSync);
            if (std::fabs(dt - k * frameLenSync) > 6.0) continue;
            res.push_back({&d, k});
        }
    }

    // take the decoded cells at this timing out of a subframe grid (nRbGrid wide): each one's channel from its reference signals, then its
    // known signals subtracted; a second round estimates each again with the others already out
    void cancelUnder(LteGrid& g, const std::vector<std::pair<const Decoded*, int>>& under, int sf, int frame, int nRbGrid) {
        std::vector<const Decoded*> ds;
        std::vector<int> sfns;
        std::vector<int> frs;
        for (const auto& u : under) if (nRbGrid == 6 || u.first->nRb == nRbGrid) { ds.push_back(u.first); sfns.push_back((u.first->sfnAt0 + frame + u.second) & 1023); frs.push_back(frame + u.second); }
        if (ds.empty()) return;
        std::vector<LteChan> chs(ds.size());
        std::vector<std::vector<LteTxPart>> ex(ds.size());
        for (int it = 0; it < 2; it++)
            for (size_t i = 0; i < ds.size(); i++) {
                const LteCellCfg cd = cfgOf(*ds[i], nRbGrid);
                if (it == 1) lteCancelKnown(g, chs[i], cd, sf, sfns[i], ds[i]->nRb, 1.f, nRbGrid > 6 ? &ex[i] : nullptr);   // put it back, estimate it again
                if (sf == 0 && nRbGrid == 6) lteChanEst(g, cd, sf, nRbGrid, 0, cd.ports, chs[i], 2, lteSymbPerSlot(cd.extCp), lteSymbPerSlot(cd.extCp) + 3);
                else lteChanEst(g, cd, sf, nRbGrid, 0, cd.ports, chs[i], it == 1 ? 2 : 0);
                double e0 = 0, e1 = 0;
                if (dbg && frame == 0 && sf == 0) for (int l = 7; l < 11; l++) for (int k = 0; k < 72; k++) e0 += std::norm(g.at(l, 6 * nRbGrid - 36 + k));
                if (nRbGrid > 6) knownParts(*ds[i], cd, frs[i], sf, sfns[i], pcfichDecode(g, chs[i], cd, sf), ex[i]);
                double q0 = 0, q1 = 0;
                const bool show = dbg && nRbGrid > 6 && sf == 5 && frame < 2 && !ex[i].empty();
                if (show) for (const auto& pt : ex[i]) for (const auto& q : pt.res) q0 += std::norm(g.at(q.first, q.second));
                lteCancelKnown(g, chs[i], cd, sf, sfns[i], ds[i]->nRb, -1.f, nRbGrid > 6 ? &ex[i] : nullptr);
                if (show) { for (const auto& pt : ex[i]) for (const auto& q : pt.res) q1 += std::norm(g.at(q.first, q.second)); fprintf(stderr, "[cell]     it %d PCI %d sfn %d: %zu parts, %.1f dB\n", it, ds[i]->pci, sfns[i], ex[i].size(), 10 * std::log10(q1 / std::max(1e-30, q0))); }
                if (dbg && frame == 0 && sf == 0) { for (int l = 7; l < 11; l++) for (int k = 0; k < 72; k++) e1 += std::norm(g.at(l, 6 * nRbGrid - 36 + k)); fprintf(stderr, "[cell]     it %d cancel PCI %d sfn %d ports %d: %.1f dB\n", it, ds[i]->pci, sfns[i], cd.ports, 10 * std::log10(e1 / std::max(1e-30, e0))); }
            }
    }

    // PBCH, from the central 6 resource blocks. hard: decoded cells at this timing are cancelled first, the channel is estimated over a wider
    // span, and the frames are combined across 40 ms periods (a weak cell, or one under a stronger one)
    MibTry pbchStage(std::vector<cf32>& ys, Track& tr, double fs0, int pci, bool ext, bool fixCfo, bool hard, LteCellResult& R) {
        LteCellCfg c6;
        c6.pci = pci; c6.extCp = ext; c6.tdd = false; c6.nRb = 6; c6.ports = 4;
        const auto pres = pbchRes(pci, ext);
        std::vector<std::array<std::vector<float>, 3>> llr;
        double rsrp6 = 0, rssi6 = 0, nv6 = 0; int nm6 = 0;
        const double frameLen = 2 * tr.slope;
        std::vector<std::pair<const Decoded*, int>> under;
        if (hard) sameTiming(fs0, tr.cfo, ext, pci, under);
        if (!under.empty()) {   // one site's sectors share the oscillator: the strong cell's carrier offset is the better measure
            const double d = under[0].first->cfo - tr.cfo;
            mixDown(ys.data(), ys.size(), d, kSyncRate);
            tr.cfo += d;
            R.found.cfoHz = tr.cfo;
        }
        LteGrid g;
        LteChan ch;
        for (int pass = fixCfo ? 0 : 1; pass < 2; pass++) {
            cf32 rot(0, 0);
            for (int f = 0;; f++) {
                const long s = std::lround(fs0 + f * frameLen);
                if ((size_t)(s + lteSubframeLen(kNfft6) + 8) > ys.size()) break;
                if (pass == 0) {   // the residual carrier offset: the reference signals of ports 0 and 1, slot 0 against slot 1 of every subframe
                    const int N = lteSymbPerSlot(ext);   // (0.5 ms apart: +-1 kHz), the whole block
                    for (int sf = 0; sf < 10; sf++) {
                        const long s2 = s + (long)sf * lteSubframeLen(kNfft6);
                        if ((size_t)(s2 + lteSubframeLen(kNfft6) + 8) > ys.size()) break;
                        lteOfdmDemod(&ys[(size_t)s2], 6, ext, kNfft6, g);
                        cf32 ra[2 * kMaxRb], rb[2 * kMaxRb];
                        for (int l : {0, N - 3}) {
                            lteCrsSeq(pci, 2 * sf, l, ext, ra);
                            lteCrsSeq(pci, 2 * sf + 1, l, ext, rb);
                            for (int p = 0; p < 2; p++)
                                for (int k = 0; k < 72; k++) {
                                    if (!lteCrsAt(c6, sf, l, k, p)) continue;
                                    const int mm = k / 6 + kMaxRb - 6;
                                    rot += (g.at(N + l, k) * std::conj(rb[mm])) * std::conj(g.at(l, k) * std::conj(ra[mm]));
                                }
                        }
                    }
                    continue;
                }
                lteOfdmDemod(&ys[(size_t)s], 6, ext, kNfft6, g);
                if (!under.empty()) {
                    double e0 = 0, e1 = 0;
                    if (dbg && f == 0) for (const auto& q : pres) e0 += std::norm(g.at(q.first, q.second));
                    cancelUnder(g, under, 0, f, 6);
                    if (dbg && f == 0) { for (const auto& q : pres) e1 += std::norm(g.at(q.first, q.second)); fprintf(stderr, "[cell]   PCI %d: %zu cells cancelled, PBCH power %.1f dB\n", pci, under.size(), 10 * std::log10(std::max(1e-30, e1 / std::max(1e-30, e0)))); }
                }
                if (hard && !under.empty()) lteChanEst(g, c6, 0, 6, 0, 4, ch, 3, lteSymbPerSlot(ext), lteSymbPerSlot(ext) + 3);
                else lteChanEst(g, c6, 0, 6, 0, 4, ch, hard ? 3 : 0);
                rsrp6 += ch.rsrp; rssi6 += ch.rssi; nv6 += ch.noiseVar; nm6++;
                std::array<std::vector<float>, 3> L;
                for (int pi = 0; pi < 3; pi++) txdRes(g, ch, pres, portsOf[pi], L[(size_t)pi]);
                llr.push_back(std::move(L));
            }
            if (pass == 0 && std::abs(rot) > 0) {
                const double d = std::arg(rot) / (2 * M_PI * 0.5e-3);
                mixDown(ys.data(), ys.size(), d, kSyncRate);
                tr.cfo += d;
                R.found.cfoHz = tr.cfo;
            }
        }
        if (nm6 && !hard) {
            R.meas.rsrpDb = (float)(10 * std::log10(std::max(1e-20, rsrp6 / nm6)));
            R.meas.rssiDb = (float)(10 * std::log10(std::max(1e-20, rssi6 / nm6)));
            R.meas.rsrqDb = (float)(10 * std::log10(std::max(1e-20, 6 * rsrp6 / std::max(1e-20, rssi6))));
            R.meas.sinrDb = (float)(10 * std::log10(std::max(1e-20, rsrp6 / std::max(1e-20, nv6))));
        }
        MibTry mt = mibDecode(llr, pci, ext);
        if (!mt.ok && hard) mt = mibCombine(llr, pci, ext);
        return mt;
    }

    // system information on SI-RNTI over the block. under: decoded cells at this timing (their reference signals and PBCH cancelled first);
    // wide: channel estimate span; hint: a SIB1 allocation to try when none of this cell's own SIB1 DCIs decoded (a sector of the same site)
    void siStage(LteCellResult& R, const LteCellCfg& c, double start, double cfo, double r, int sfnAt0, int wide,
                       const std::vector<std::pair<const Decoded*, int>>& under, const LteDciSi* hint, Decoded* rec) {
        const int nRb = c.nRb, pci = c.pci;
        const int nfft = lteNfft(nRb);
        const double rate = lteRate(nfft);
        const double halfBw = nRb * 90e3 + 30e3;
        const double frameIn = 0.01 * fs * r;
        const int nFrames = (int)std::floor(((double)n - start - 64) / frameIn);
        if (nFrames < 1) return;
        const size_t sfLen = (size_t)lteSubframeLen(nfft);
        const std::vector<cf32> z = resampleBlock(x, n, fs, rate, offsetHz + cfo, 2 * halfBw, start, (size_t)nFrames * 10 * sfLen + 16, r);
        double rsrp = 0, rssi = 0, nv = 0; int nm = 0;
        std::vector<Bits> seen;
        LteGrid g;
        LteChan ch;
        struct Rep { LteGrid g; LteChan ch; int sfn, cfi; };
        std::vector<Rep> reps;              // subframe 5 of the even frames: SIB1 repetitions, combined below if no single one decoded
        std::vector<LteDciSi> sib1Dcis;
        bool sib1Got = false;
        for (int fr = 0; fr < nFrames; fr++)
            for (int sf = 0; sf < 10; sf++) {
                const int sfn = (sfnAt0 + fr) & 1023;
                lteOfdmDemod(&z[((size_t)fr * 10 + (size_t)sf) * sfLen], nRb, c.extCp, nfft, g);
                cancelUnder(g, under, sf, fr, nRb);
                lteChanEst(g, c, sf, nRb, 0, c.ports, ch, wide);
                rsrp += ch.rsrp; rssi += ch.rssi; nv += ch.noiseVar; nm++;
                // PCFICH
                const LteControl c1 = lteControl(c, 1, sf);
                std::vector<std::pair<int, int>> res;
                for (const auto& rg : c1.pcfich) for (int i = 0; i < 4; i++) res.push_back({rg.l, rg.k[i]});
                std::vector<float> L;
                txdRes(g, ch, res, c.ports, L);
                if (L.size() < 32) continue;
                const Bits pcs = gold((uint32_t)((sf + 1) * (2 * pci + 1) * 512 + pci), 32);
                int cfi = 1; float bc = -1e30f;
                for (int k = 0; k < 3; k++) {
                    float s = 0;
                    for (int i = 0; i < 32; i++) s += (pcs[(size_t)i] ? -L[(size_t)i] : L[(size_t)i]) * (kCfiCode[k][i] ? -1.f : 1.f);
                    if (s > bc) { bc = s; cfi = k + 1; }
                }
                R.cfiSeen = cfi;
                if (sf == 5 && sfn % 2 == 0) reps.push_back({g, ch, sfn, cfi});
                // PDCCH: the quadruplets in their order, descrambled
                const LteControl ctl = lteControl(c, cfi, sf);
                const int Mq = (int)ctl.pdcchQuad.size();
                std::vector<float> pl;
                pl.reserve((size_t)Mq * 8);
                for (int q = 0; q < Mq; q++) {
                    const LteReg& rg = ctl.pdcchQuad[(size_t)q];
                    std::vector<std::pair<int, int>> rq;
                    for (int i = 0; i < 4; i++) rq.push_back({rg.l, rg.k[i]});
                    std::vector<float> lq;
                    txdRes(g, ch, rq, c.ports, lq);
                    lq.resize(8, 0.f);
                    pl.insert(pl.end(), lq.begin(), lq.end());
                }
                const Bits dcs = gold((uint32_t)(sf * 512 + pci), pl.size());
                for (size_t i = 0; i < pl.size(); i++) if (dcs[i]) pl[i] = -pl[i];
                // the common search space: aggregation 4 (4 candidates) and 8 (2)
                std::vector<DciFound> dcis;
                for (int L2 : {4, 8}) {
                    const int nPos = ctl.nCce / L2;
                    if (nPos <= 0) continue;
                    const int nc = L2 == 4 ? 4 : 2;
                    for (int m = 0; m < nc; m++) {
                        const int cce = L2 * (m % nPos);
                        const size_t E = (size_t)(72 * L2), off = (size_t)(72 * cce);
                        if (off + E > pl.size()) continue;
                        for (int fmt = 0; fmt < 2; fmt++) {
                            const int sz = fmt == 0 ? lteDci1ASize(nRb) : lteDci1CSize(nRb);
                            const size_t K = (size_t)(sz + 16);
                            const std::vector<float> d = convRateRecover(&pl[off], E, K);
                            const Bits b = viterbiTb(d.data(), K);
                            uint32_t par = 0;
                            for (size_t i = (size_t)sz; i < K; i++) par = (par << 1) | b[i];
                            if ((crc(b.data(), (size_t)sz, kCrc16, 16) ^ par) != kSiRnti) continue;
                            if (agreement(&pl[off], convRateMatch(convEncode(b), E)) < 0.3f) continue;
                            Bits p(b.begin(), b.begin() + sz);
                            bool again = false;
                            for (const auto& x2 : dcis) if (x2.payload == p) again = true;
                            if (again) continue;
                            DciFound df;
                            if (!lteDciParse(p, nRb, fmt, df.d)) { R.dciFail++; continue; }
                            df.payload = p;
                            df.cce = cce; df.L = L2;
                            df.coded = convRateMatch(convEncode(b), E);
                            if (rec) rec->dcis.push_back({fr, sf, cfi, cce, df.coded});
                            dcis.push_back(df);
                        }
                    }
                }
                // PDSCH of each SI-RNTI allocation
                for (const auto& df : dcis) {
                    R.dciSeen++;
                    std::vector<int> p0, p1;
                    ltePrbs(df.d, nRb, p0, p1);
                    bool okPrb = !p0.empty();
                    for (int v : p0) if (v < 0 || v >= nRb) okPrb = false;
                    for (int v : p1) if (v < 0 || v >= nRb) okPrb = false;
                    if (!okPrb) { R.dciFail++; continue; }
                    if (sf == 5 && sfn % 2 == 0) sib1Dcis.push_back(df.d);
                    const auto pres2 = ltePdschRes(c, sf, cfi, p0, p1);
                    std::vector<float> e;
                    txdRes(g, ch, pres2, c.ports, e);
                    const int NL = c.ports > 1 ? 2 : 1;
                    const size_t G = 2 * pres2.size(), E = (size_t)(NL * 2) * (G / (size_t)(NL * 2));
                    if (e.size() < E || E == 0) continue;
                    e.resize(E);
                    const Bits scs = gold(((uint32_t)kSiRnti << 14) + ((uint32_t)sf << 9) + (uint32_t)pci, E);
                    for (size_t i = 0; i < E; i++) if (scs[i]) e[i] = -e[i];
                    const int B = df.d.tbs + 24, K = turboSizeFor(B), F = K - B;
                    if (K == 0) continue;
                    std::vector<int> rvs;
                    if (df.d.format == 0) rvs = {df.d.rv};
                    else rvs = {0, 2, 3, 1};
                    bool got = false;
                    for (int rv : rvs) {
                        std::vector<float> d012;
                        turboRateRecover(e.data(), E, K, F, rv, d012);
                        Bits tb;
                        struct Ck { int F; } ck{F};
                        auto chk = [](const Bits& b, void* p) {
                            const int f = static_cast<Ck*>(p)->F;
                            Bits t(b.begin() + f, b.end());
                            return checkCrc(t, kCrc24A, 24);
                        };
                        if (!turboDecode(d012, K, F, 8, tb, chk, &ck)) continue;
                        Bits msg(tb.begin() + F, tb.begin() + F + df.d.tbs);
                        got = true;
                        if (sf == 5 && sfn % 2 == 0) {
                            sib1Got = true;
                            if (rec && rec->sib1Tb.empty()) { rec->haveSib1 = true; rec->sib1 = df.d; rec->sib1Tb = msg; }
                        }
                        bool again = false;
                        for (const auto& s2 : seen) if (s2 == msg) again = true;
                        if (!again) {
                            seen.push_back(msg);
                            LteSiMsg sm;
                            sm.sfn = sfn; sm.subframe = sf; sm.bits = msg; sm.format = df.d.format;
                            R.si.push_back(std::move(sm));
                        }
                        break;
                    }
                    if (!got) R.tbFail++;
                }
            }
        // SIB1 over its repetitions: the same transport block in subframe 5 of every even frame, the redundancy version following the SFN
        // (36.321 5.3.1: rv = ceil(3k/2) mod 4, k = (SFN/2) mod 4). The soft bits of each repetition go into one circular buffer.
        if (!sib1Got && reps.size() >= 2) {
            std::vector<LteDciSi> hyps = sib1Dcis;
            if (hint) hyps.push_back(*hint);
            if (dbg) fprintf(stderr, "[cell]   PCI %d SIB1 combining: %zu repetitions, %zu allocations, %zu cells cancelled, %d DCIs, SINR %.1f\n", pci, reps.size(), hyps.size(), under.size(), R.dciSeen, 10 * std::log10(std::max(1e-20, rsrp / std::max(1e-20, nv))));
            for (size_t h = 0; h < hyps.size() && h < 4 && !sib1Got; h++) {
                const LteDciSi& dd = hyps[h];
                bool dupH = false;
                for (size_t q = 0; q < h; q++) if (hyps[q].tbs == dd.tbs && hyps[q].rbStart == dd.rbStart && hyps[q].lCrb == dd.lCrb && hyps[q].distributed == dd.distributed) dupH = true;
                if (dupH) continue;
                std::vector<int> p0, p1;
                ltePrbs(dd, nRb, p0, p1);
                bool okPrb = !p0.empty();
                for (int v : p0) if (v < 0 || v >= nRb) okPrb = false;
                for (int v : p1) if (v < 0 || v >= nRb) okPrb = false;
                const int B = dd.tbs + 24, K = turboSizeFor(B), F = K - B;
                if (!okPrb || K == 0) continue;
                std::vector<float> acc;
                int nAdd = 0;
                for (const Rep& rp : reps) {
                    const auto pres2 = ltePdschRes(c, 5, rp.cfi, p0, p1);
                    std::vector<float> e;
                    txdRes(rp.g, rp.ch, pres2, c.ports, e);
                    const int NL = c.ports > 1 ? 2 : 1;
                    const size_t G = 2 * pres2.size(), E = (size_t)(NL * 2) * (G / (size_t)(NL * 2));
                    if (e.size() < E || E == 0) continue;
                    e.resize(E);
                    const Bits scs = gold(((uint32_t)kSiRnti << 14) + ((uint32_t)5 << 9) + (uint32_t)pci, E);
                    for (size_t i = 0; i < E; i++) if (scs[i]) e[i] = -e[i];
                    const int k = (rp.sfn / 2) % 4, rv = ((3 * k + 1) / 2) % 4;
                    turboRateRecover(e.data(), E, K, F, rv, acc);
                    nAdd++;
                }
                if (nAdd < 2) continue;
                Bits tb;
                struct Ck { int F; } ck{F};
                auto chk = [](const Bits& b, void* p) {
                    const int f = static_cast<Ck*>(p)->F;
                    Bits t(b.begin() + f, b.end());
                    return checkCrc(t, kCrc24A, 24);
                };
                if (!turboDecode(acc, K, F, 10, tb, chk, &ck)) continue;
                sib1Got = true;
                LteSiMsg sm;
                sm.sfn = reps[0].sfn; sm.subframe = 5; sm.bits.assign(tb.begin() + F, tb.begin() + F + dd.tbs); sm.format = dd.format;
                if (rec && rec->sib1Tb.empty()) { rec->haveSib1 = true; rec->sib1 = dd; rec->sib1Tb = sm.bits; }
                if (dbg) fprintf(stderr, "[cell]   PCI %d SIB1 from %d repetitions combined\n", pci, nAdd);
                R.si.push_back(std::move(sm));
            }
        }
        if (nm) {
            R.meas.rsrpDb = (float)(10 * std::log10(std::max(1e-20, rsrp / nm)));
            R.meas.rssiDb = (float)(10 * std::log10(std::max(1e-20, rssi / nm)));
            R.meas.rsrqDb = (float)(10 * std::log10(std::max(1e-20, nRb * rsrp / std::max(1e-20, rssi))));
            R.meas.sinrDb = (float)(10 * std::log10(std::max(1e-20, rsrp / std::max(1e-20, nv))));
        }
    }

    // a cell whose MIB decoded: its system information, then it is one of the decoded cells (outIdx: its place in out if listed already)
    void accept(LteCellResult& R, const MibTry& mt, double fs0, const Track& tr, bool hardMib, int outIdx) {
        const int pci = R.found.pci;
        const bool ext = R.found.extCp;
        R.mib = mt.mib;
        R.mib.frameStart = R.found.frameStart;
        const int sfnAt0 = (mt.mib.sfn - mt.frame + 1024 * 4) & 1023;   // SFN of the first whole frame in the block
        R.mib.sfn = sfnAt0;
        Decoded D;
        D.pci = pci; D.ext = ext; D.nRb = mt.mib.nRb; D.ports = mt.mib.ports; D.phichDur = mt.mib.phichDur; D.phichRes = mt.mib.phichRes; D.mibTail = mt.mib.mibTail;
        D.fs0 = fs0; D.startIn = R.found.frameStart; D.cfo = tr.cfo; D.sfnAt0 = sfnAt0;
        // system information: needs the whole carrier inside the block
        const int nRb = mt.mib.nRb;
        const double halfBw = nRb * 90e3 + 30e3;
        if (o.wantSi && !R.found.tdd && std::fabs(offsetHz) + halfBw < fs / 2) {
            LteCellCfg c;
            c.pci = pci; c.extCp = ext; c.tdd = false; c.nRb = nRb; c.ports = mt.mib.ports; c.phichDur = mt.mib.phichDur; c.phichRes = mt.mib.phichRes; c.mibTail = mt.mib.mibTail;
            std::vector<std::pair<const Decoded*, int>> under;
            sameTiming(fs0, tr.cfo, ext, pci, under);
            const LteDciSi* hint = nullptr;
            for (const auto& u : under) if (u.first->haveSib1 && u.first->nRb == nRb) { hint = &u.first->sib1; break; }
            if (hardMib) siStage(R, c, R.found.frameStart, under.empty() ? tr.cfo : under[0].first->cfo, tr.slope / kHalf, sfnAt0, 2, under, hint, &D);
            else {
                siStage(R, c, R.found.frameStart, tr.cfo, tr.slope / kHalf, sfnAt0, 0, {}, nullptr, &D);
                if (R.si.empty() && !under.empty()) {   // nothing read: again with the cells at its timing cancelled, their carrier offset (one
                    R.si.clear();                       // site's oscillator) and the wider channel estimate
                    siStage(R, c, R.found.frameStart, under[0].first->cfo, tr.slope / kHalf, sfnAt0, 2, under, hint, &D);
                }
            }
        }
        D.slope = tr.slope;
        D.outIdx = outIdx >= 0 ? outIdx : (int)out.size();
        decoded.push_back(D);
        if (outIdx >= 0) out[(size_t)outIdx] = std::move(R);
        else out.push_back(std::move(R));
    }

    // the PSS candidates, one after the other (see below for the leftovers)
    void searchPasses() {
    size_t knownBefore = 0;
    for (int pass = 0; pass < 4; pass++) {
    if (pass >= 1 && known.size() == knownBefore) break;
    knownBefore = known.size();
    for (const Cand& cd : cands) {
        if ((int)out.size() >= o.maxCells) break;
        Track tr;
        if (!pssTrack(y, cd, tr)) continue;
        std::vector<cf32> ys = y;
        mixDown(ys.data(), ys.size(), tr.cfo, kSyncRate);
        const Track tr0 = tr;
        const std::vector<cf32> ys0 = ys;
        const std::vector<SssResult> sss = sssDetect(ys, cd.nid2, tr, known);
        for (const SssResult& ss : sss) {
        if ((int)out.size() >= o.maxCells) break;
        tr = tr0; ys = ys0;
        if (dbg) fprintf(stderr, "[cell]   track cfo %.0f slope %.3f halves %d; SSS nid1 %d score %.2f sig %.1f ext %d tdd %d\n", tr.cfo, tr.slope, tr.nHalf, ss.nid1, ss.score, ss.sig, ss.ext, ss.tdd);
        if (!ss.ok) continue;
        const int pci = 3 * ss.nid1 + cd.nid2;
        bool dup = false;
        for (const auto& r : out) if (r.found.pci == pci) dup = true;
        for (const auto& p : pending) if (p.R.found.pci == pci) dup = true;
        if (dup) continue;
        // the first frame start: the PSS of subframe 0 (FDD: end of slot 0; TDD: symbol 2 of subframe 1)
        const double frameOff = !ss.tdd ? 832.0 : ss.ext ? 2272.0 : 2194.0;
        int m0 = ss.sf0Parity;   // the half frames of subframe 0 have this parity
        double fs0 = tr.t0 + tr.slope * m0 - frameOff;
        const double frameLen = 2 * tr.slope;
        while (fs0 - frameLen >= 0) fs0 -= frameLen;
        while (fs0 < 0) fs0 += frameLen;
        LteCellResult R;
        R.found.pci = pci;
        R.found.extCp = ss.ext;
        R.found.tdd = ss.tdd;
        R.found.cfoHz = tr.cfo;
        R.found.clockPpm = (tr.slope / kHalf - 1) * 1e6;
        R.found.pssDb = cd.db;
        R.found.sssScore = ss.score;
        R.found.sssSig = ss.sig;
        R.found.frameStart = fs0 / toSync;
        MibTry mt = pbchStage(ys, tr, fs0, pci, ss.ext, true, false, R);
        bool hardMib = false;
        if (!mt.ok && ss.sig >= 3.0f) { mt = pbchStage(ys, tr, fs0, pci, ss.ext, false, true, R); hardMib = mt.ok; }
        if (dbg) fprintf(stderr, "[cell]   PCI %d MIB %d%s: %d RB %d ports sfn %d, RSRP %.1f SINR %.1f fs0 %.1f cfo %.0f\n", pci, mt.ok, hardMib ? " (combined)" : "", mt.mib.nRb, mt.mib.ports, mt.mib.sfn, R.meas.rsrpDb, R.meas.sinrDb, fs0, tr.cfo);
        if (!mt.ok) {
            // the PBCH did not decode (too weak, or under a stronger cell): a clear SSS still lists the cell, without its MIB
            int idx = -1;
            if (ss.sig >= kSssListAlone) { remember(ss, cd.nid2, tr0); idx = (int)out.size(); out.push_back(R); }
            if (!ss.tdd && ss.sig >= 3.0f) pending.push_back({R, fs0, tr, idx});
            continue;
        }
        accept(R, mt, fs0, tr, hardMib, -1);
        remember(ss, cd.nid2, tr0);
        }
    }
    }
    }

    // the leftovers: cells whose PBCH did not decode, again with everything decoded meanwhile cancelled
    void retryLeftovers() {
    int tries = 0;
    auto progress = [&]() { size_t v = decoded.size(); for (const auto& r : out) if (hasSib1(r)) v++; return v; };
    for (int outer = 0; outer < 3; outer++) {
        const size_t before = progress();
        // the cells whose MIB did not decode, again with every decoded cell at their timing cancelled (one decoded late may free another)
        for (int round = 0; round < 3; round++) {
            bool changed = false;
            for (Pending& p : pending) {
                if (p.done) continue;
                std::vector<std::pair<const Decoded*, int>> under;
                sameTiming(p.fs0, p.tr.cfo, p.R.found.extCp, p.R.found.pci, under);
                if (under.empty()) continue;
                std::vector<cf32> ys = y;
                mixDown(ys.data(), ys.size(), p.tr.cfo, kSyncRate);
                Track tr = p.tr;
                LteCellResult R = p.R;
                const MibTry mt = pbchStage(ys, tr, p.fs0, R.found.pci, R.found.extCp, false, true, R);
                if (dbg) fprintf(stderr, "[cell]   PCI %d MIB again under %zu cells: %d\n", R.found.pci, under.size(), mt.ok);
                if (!mt.ok) continue;
                p.done = changed = true;
                if (p.outIdx < 0 && (int)out.size() >= o.maxCells + 4) continue;
                accept(R, mt, p.fs0, tr, true, p.outIdx);
            }
            if (!changed) break;
        }
        // still without a MIB, under a decoded cell at the same frame timing (a synchronised network): the same carrier has the same bandwidth
        // and PHICH set-up, and the SFN follows from the timing. The antenna ports are tried (the decoded cell's count first); the CRCs decide.
        for (Pending& p : pending) {
            if (!o.wantSi || tries >= 6) break;
            if (p.done) continue;
            std::vector<std::pair<const Decoded*, int>> under;
            sameTiming(p.fs0, p.tr.cfo, p.R.found.extCp, p.R.found.pci, under);
            if (under.empty()) continue;
            const Decoded& d = *under[0].first;
            const double halfBw = d.nRb * 90e3 + 30e3;
            if (std::fabs(offsetHz) + halfBw >= fs / 2) continue;
            tries++;
            const int sfnAt0 = (d.sfnAt0 - under[0].second + 1024) & 1023;
            const double cfo = d.cfo;   // the site's oscillator
            const int order[3] = {d.ports, d.ports == 2 ? 4 : 2, d.ports == 1 ? 4 : 1};   // the site's own count first
            for (int ports : order) {
                LteCellResult R = p.R;
                LteCellCfg c;
                c.pci = R.found.pci; c.extCp = R.found.extCp; c.tdd = false; c.nRb = d.nRb; c.ports = ports; c.phichDur = d.phichDur; c.phichRes = d.phichRes; c.mibTail = d.mibTail;
                Decoded D2;
                siStage(R, c, R.found.frameStart, cfo, p.tr.slope / kHalf, sfnAt0, 2, under, d.haveSib1 ? &d.sib1 : nullptr, &D2);
                if (dbg) fprintf(stderr, "[cell]   PCI %d borrows the MIB of PCI %d, %d ports: %zu SI\n", c.pci, d.pci, ports, R.si.size());
                if (R.si.empty()) continue;
                R.found.cfoHz = cfo;
                R.mib.ok = true; R.mib.nRb = d.nRb; R.mib.ports = ports; R.mib.phichDur = d.phichDur; R.mib.phichRes = d.phichRes; R.mib.mibTail = d.mibTail;
                R.mib.sfn = sfnAt0; R.mib.frameStart = R.found.frameStart;
                p.done = true;
                D2.pci = c.pci; D2.ext = c.extCp; D2.nRb = c.nRb; D2.ports = ports; D2.phichDur = c.phichDur; D2.phichRes = c.phichRes; D2.mibTail = c.mibTail;
                D2.fs0 = p.fs0; D2.startIn = R.found.frameStart; D2.cfo = cfo; D2.sfnAt0 = sfnAt0; D2.slope = p.tr.slope;
                if (p.outIdx >= 0) { D2.outIdx = p.outIdx; out[(size_t)p.outIdx] = std::move(R); }
                else if ((int)out.size() < o.maxCells + 4) { D2.outIdx = (int)out.size(); out.push_back(std::move(R)); }
                if (D2.outIdx >= 0 && decoded.size() < decoded.capacity()) decoded.push_back(D2);
                break;
            }
        }
        // the decoded cells still without their SIB1, again with every cell decoded since cancelled (each SIB1 read takes its PDSCH out of the
        // others' way)
        for (int round = 0; round < 3 && o.wantSi; round++) {
            bool changed = false;
            for (size_t di = 0; di < decoded.size(); di++) {
                Decoded& d = decoded[di];
                if (d.outIdx < 0 || d.outIdx >= (int)out.size() || hasSib1(out[(size_t)d.outIdx])) continue;
                const double halfBw = d.nRb * 90e3 + 30e3;
                if (out[(size_t)d.outIdx].found.tdd || std::fabs(offsetHz) + halfBw >= fs / 2) continue;
                std::vector<std::pair<const Decoded*, int>> under;
                sameTiming(d.fs0, d.cfo, d.ext, d.pci, under);
                if (under.empty()) continue;
                LteCellResult R = out[(size_t)d.outIdx];
                R.si.clear();
                const LteCellCfg c = cfgOf(d, d.nRb);
                const LteDciSi* hint = nullptr;
                for (const auto& u : under) if (u.first->haveSib1 && u.first->nRb == d.nRb) { hint = &u.first->sib1; break; }
                siStage(R, c, d.startIn, under[0].first->cfo, d.slope / kHalf, d.sfnAt0, 2, under, hint, &d);
                if (dbg) fprintf(stderr, "[cell]   PCI %d SI again under %zu cells: %zu SI\n", d.pci, under.size(), R.si.size());
                if (!hasSib1(R)) continue;
                out[(size_t)d.outIdx] = std::move(R);
                changed = true;
            }
            if (!changed) break;
        }
        if (progress() == before) break;
    }
    }
};

}   // namespace

// All the LTE cells in a block of samples that was recorded at fs with the radio offsetHz away from the channel (see LteSearchOpts).
std::vector<LteCellResult> lteSearchDecode(const cf32* x, size_t n, double fs, double offsetHz, const LteSearchOpts& o) {
    std::vector<LteCellResult> out;
    if (!x || n == 0 || fs < kSyncRate * 0.99) return out;
    const double toSync = kSyncRate / fs;
    const size_t nSync = (size_t)((double)n * toSync);
    if (nSync < (size_t)(3 * kHalf)) return out;
    LteSearch s(x, n, fs, offsetHz, o, toSync, resampleBlock(x, n, fs, kSyncRate, offsetHz, 1.3e6, 0, nSync));
    s.cands = pssSearch(s.y, o.maxCfoHz, o.minPssDb);
    if (dbg) for (const auto& c : s.cands) fprintf(stderr, "[cell] off %.0f: PSS nid2 %d cfo %.0f t %.0f %.1f dB\n", offsetHz, c.nid2, c.cfo, c.t0, c.db);
    s.searchPasses();
    s.retryLeftovers();
    return s.out;
}

} // namespace dect2::cell
