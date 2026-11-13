// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/diag.h"
#include "dect2/common/stage_prof.h"
#include "dect2/atsc3/atsc3_frame.h"
#include "dect2/atsc3/atsc3_ti.h"
#include "dect2/atsc3/atsc3_bb.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace dect2 {
namespace atsc3 {

BicmConfig plpBicm(const FramePlp& p) {
    BicmConfig c;
    c.nInner = 0;
    if (p.fecType < 0 || p.fecType > 5 || p.mod < 0 || p.mod > 5 || p.cod < 0 || p.cod > 11) return c;
    c.nInner = (p.fecType & 1) ? 64800 : 16200;
    c.outer = p.fecType <= 1 ? 0 : p.fecType <= 3 ? 1 : 2;
    c.bitsPerCell = 2 + 2 * p.mod;
    c.rate15 = 2 + p.cod;
    if (c.nInner == 16200 && c.bitsPerCell > 8) c.nInner = 0;
    return c;
}

static SubframeParams subframeFrom(const L1Basic& l1, const L1Detail& d) {
    SubframeParams s;
    s.fftSize = fftFromCode(l1.firstSubFftSize);
    s.cred = l1.firstSubReducedCarriers;
    s.guard = guardFromCode(l1.firstSubGuardInterval);
    s.spPattern = l1.firstSubScatteredPilotPattern;
    s.spBoost = l1.firstSubScatteredPilotBoost;
    s.numSymbols = l1.firstSubNumOfdmSymbols + 1;
    s.sbsFirst = l1.firstSubSbsFirst;
    s.sbsLast = l1.firstSubSbsLast;
    if (!d.subframes.empty()) {
        s.freqInterleaver = d.subframes[0].frequencyInterleaver;
        s.sbsNullCells = d.subframes[0].sbsNullCells;
    }
    s.fiOffset = l1.preambleNumSymbols + 1;   // the interleaver counts the Preamble symbols of the first subframe
    return s;
}

std::vector<cf32> buildFrame(const FrameSetup& fs, const std::vector<FramePlp>& plps, L1Basic* l1Out, int* freeOut) {
    L1Basic l1;
    l1.preambleNumSymbols = fs.preambleSymbols - 1;
    l1.preambleReducedCarriers = fs.preambleSymbols > 1 ? fs.preambleReduced : 0;
    l1.l1DetailFecType = fs.l1DetailMode - 1;
    l1.firstSubFftSize = fs.fftCode; l1.firstSubReducedCarriers = fs.reduced; l1.firstSubGuardInterval = fs.guardCode;
    l1.firstSubNumOfdmSymbols = fs.numSymbols - 1; l1.firstSubScatteredPilotPattern = fs.spPattern; l1.firstSubScatteredPilotBoost = fs.spBoost;
    l1.firstSubSbsFirst = fs.sbsFirst; l1.firstSubSbsLast = fs.sbsLast;
    l1.numSubframes = 0;
    L1Detail d;
    L1DetailSubframe sf;
    sf.frequencyInterleaver = fs.freqInterleaver;
    sf.sbsNullCells = fs.sbsNullCells;
    for (auto& p : plps) {
        L1DetailPlp q;
        q.id = p.id; q.fecType = p.fecType; q.mod = p.mod; q.cod = p.cod; q.tiMode = 0; q.layer = 0; q.type = 0;
        sf.plps.push_back(q);
    }
    d.subframes = {sf};
    d.bsid = 0x1234;
    // sizes first: they do not depend on the PLP positions
    int sizeBytes = l1DetailSizeBytes(l1, d);
    l1.l1DetailSizeBytes = sizeBytes;
    l1.l1DetailTotalCells = l1DetailCells(sizeBytes, fs.l1DetailMode);
    const int prefix = preambleLeftoverCells(fs.bs, l1);
    if (prefix < 0) {
        DIAG_DEBUG(524624, "An ATSC 3.0 frame cannot be built: the preamble leaves no room for the L1 signalling, so no frame is produced.");
        return {};
    }
    SubframeParams sp = subframeFrom(l1, d);
    sp.freqInterleaver = fs.freqInterleaver; sp.sbsNullCells = fs.sbsNullCells;
    const long total = subframeTotalCells(sp, prefix);
    // PLP cells and positions: one after the other, starting after the cells of the Preamble
    std::vector<std::pair<PlpAlloc, std::vector<cf32>>> placed;
    long pos = prefix;
    for (size_t i = 0; i < plps.size(); i++) {
        BicmConfig bc = plpBicm(plps[i]);
        Bicm bicm(bc);
        if (!bicm.ok()) {
            DIAG_DEBUG_EVERY(185531, 10.0, "An ATSC 3.0 frame cannot be built: PLP %d uses a modulation and code rate combination that is not supported, so no frame is produced.", plps[i].id);
            return {};
        }
        std::vector<cf32> cells;
        for (auto& pk : plps[i].bbPackets) {
            if ((int)pk.size() * 8 != bicm.kPayload()) {
                DIAG_DEBUG_EVERY(752567, 10.0, "An ATSC 3.0 frame cannot be built: a baseband packet of PLP %d has %zu bytes but its FEC block carries %d bits, so no frame is produced.", plps[i].id, pk.size(), bicm.kPayload());
                return {};
            }
            auto bits = bytesToBits(pk);
            bbScramble(bits);
            auto c = bicm.encode(bits);
            cells.insert(cells.end(), c.begin(), c.end());
        }
        PlpAlloc a;
        a.start = (int)pos; a.size = (int)cells.size();
        d.subframes[0].plps[i].start = a.start; d.subframes[0].plps[i].size = a.size;
        pos += a.size;
        placed.push_back({a, cells});
    }
    if (pos > total) {
        DIAG_DEBUG(839353, "An ATSC 3.0 frame cannot be built: the PLPs need %ld cells but the subframe has room for %ld, so no frame is produced.", pos, total);
        return {};
    }
    if (freeOut) *freeOut = (int)(total - pos);
    auto all = multiplexSubframe(sp, prefix, placed);
    std::vector<cf32> leftover(all.begin(), all.begin() + prefix), active(all.begin() + prefix, all.end());
    int got = 0;
    auto pre = buildPreamble(fs.bs, l1, d, 1, &got, &leftover);
    if (pre.empty() || got != prefix) {
        DIAG_DEBUG(660212, "An ATSC 3.0 frame cannot be built: the preamble could not be generated (%d of %d leftover cells fitted), so no frame is produced.", got, prefix);
        return {};
    }
    auto body = modulateSubframe(sp, active);
    pre.insert(pre.end(), body.begin(), body.end());
    if (l1Out) *l1Out = l1;
    return pre;
}

size_t frameLengthSamples(const Bootstrap& bs, const L1Basic& l1, const L1Detail& d) {
    PreambleParams pp;
    if (!preambleParams(bs.preambleStructure, pp)) return 0;
    size_t total = (size_t)(pp.fftSize + pp.guard) * (l1.preambleNumSymbols + 1);
    for (size_t i = 0; i < d.subframes.size() && (int)i <= l1.numSubframes; i++) {
        int fft, guard, syms;
        if (i == 0) { fft = fftFromCode(l1.firstSubFftSize); guard = guardFromCode(l1.firstSubGuardInterval); syms = l1.firstSubNumOfdmSymbols + 1; }
        else { fft = fftFromCode(d.subframes[i].fftSize); guard = guardFromCode(d.subframes[i].guardInterval); syms = d.subframes[i].numOfdmSymbols + 1; }
        if (!fft || !guard) return 0;
        total += (size_t)(fft + guard) * syms;
    }
    return total;
}

// The parameters of subframe i of the frame (the first one comes from L1-Basic, the others from L1-Detail).
static bool subframeParams(const L1Basic& l1, const L1Detail& d, int i, SubframeParams& s) {
    if (i == 0) { s = subframeFrom(l1, d); return s.fftSize != 0 && s.guard != 0; }
    if (i >= (int)d.subframes.size()) return false;
    const L1DetailSubframe& q = d.subframes[(size_t)i];
    s = SubframeParams();
    s.fftSize = fftFromCode(q.fftSize);
    s.cred = q.reducedCarriers;
    s.guard = guardFromCode(q.guardInterval);
    s.spPattern = q.scatteredPilotPattern;
    s.spBoost = q.scatteredPilotBoost;
    s.numSymbols = q.numOfdmSymbols + 1;
    s.sbsFirst = q.sbsFirst;
    s.sbsLast = q.sbsLast;
    s.freqInterleaver = q.frequencyInterleaver;
    s.sbsNullCells = q.sbsNullCells;
    s.fiOffset = 0;   // the interleaver counts the symbols from the start of each later subframe (A/322 7.3)
    return s.fftSize != 0 && s.guard != 0;
}

// Decodes FEC blocks of one PLP (cells of each block in `blocks`) in batches with the 8-bit decoder, into baseband packets.
static void decodeBlocks(const Bicm& bicm, const std::vector<std::vector<cf32>>& blocks, float noise, PlpResult& pr, const std::vector<std::vector<float>>* weights = nullptr) {
    const size_t bs = 16;
    for (size_t from = 0; from < blocks.size(); from += bs) {
        const size_t to = std::min(blocks.size(), from + bs);
        std::vector<const cf32*> in;
        std::vector<const float*> win;
        for (size_t i = from; i < to; i++) { in.push_back(blocks[i].data()); if (weights && i < weights->size()) win.push_back((*weights)[i].data()); }
        std::vector<std::vector<uint8_t>> pl;
        std::vector<char> good;
        bicm.decodeBatch(in, noise, pl, good, win.size() == in.size() ? &win : nullptr);
        for (size_t i = 0; i < to - from; i++) {
            pr.blocks++;
            pr.ok.push_back(good[i] != 0);
            if (good[i]) {
                bbScramble(pl[i]);
                pr.packets.push_back(bitsToBytes(pl[i]));
                pr.blocksOk++;
            }
        }
    }
}

FrameResult decodeFrame(const cf32* x, size_t n, const Bootstrap& bs) {
    FrameResult r;
    { DECT2_STAGE("a3.preamble"); r.preamble = decodePreamble(x, n, bs); }
    DECT2_STAGE("a3.cells");
    if (!r.preamble.basicOk || !r.preamble.detailOk || r.preamble.detail.subframes.empty()) {
        DIAG_DEBUG_EVERY(401568, 5.0, "The preamble of an ATSC 3.0 frame could not be read (L1-Basic %s, L1-Detail %s), so the frame is not decoded.", r.preamble.basicOk ? "ok" : "failed", r.preamble.detailOk ? "ok" : "failed");
        return r;
    }
    const L1Basic& l1 = r.preamble.basic;
    const L1Detail& d = r.preamble.detail;
    size_t pos = r.preamble.samples;   // the first sample of the subframe being read
    const int nSub = std::min<int>(l1.numSubframes + 1, (int)d.subframes.size());
    for (int si = 0; si < nSub; si++) {
        SubframeParams s;
        if (!subframeParams(l1, d, si, s)) {
            DIAG_DEBUG_EVERY(889278, 5.0, "Subframe %d of an ATSC 3.0 frame has no valid FFT size or guard interval in its L1 signalling, so the rest of the frame is not decoded.", si);
            return r;
        }
        if (si == 0) r.subframe = s;
        const size_t symLen = (size_t)s.fftSize + s.guard;
        if (n < pos + symLen * s.numSymbols) {
            DIAG_DEBUG_EVERY(220786, 5.0, "Subframe %d of an ATSC 3.0 frame is cut off (%zu samples present, %zu needed), so %s.", si, n - std::min(n, pos), symLen * (size_t)s.numSymbols, si == 0 ? "the frame is not decoded" : "only the earlier subframes are decoded");
            if (si == 0) return r;
            break;
        }   // a later subframe that is cut off: what was read before it is still good
        std::vector<cf32> active;
        std::vector<float> nv, wv;
        // 4.0 (docs/SENSITIVITY-4.0-X3.md): the channel state of each cell goes to the soft bits (DECT2_ATSC3_NOCSI=1: one noise for every cell, as 3.9)
        const char* noCsiEnv = getenv("DECT2_ATSC3_NOCSI");
        const bool csi = !(noCsiEnv && *noCsiEnv && *noCsiEnv != '0');
        if (!demodulateSubframe(s, x + pos, n - pos, active, &nv, csi ? &wv : nullptr)) {
            DIAG_DEBUG_EVERY(612319, 5.0, "Subframe %d of an ATSC 3.0 frame could not be demodulated, so the rest of the frame is not decoded.", si);
            return r;
        }
        pos += symLen * s.numSymbols;
        // the data cells of the first subframe start in the last Preamble symbol
        std::vector<cf32> all;
        std::vector<float> noise, weight;
        if (si == 0) { all = r.preamble.leftover; noise.assign(all.size(), r.preamble.leftoverNoise); weight.assign(all.size(), 1.f); }
        all.insert(all.end(), active.begin(), active.end());
        noise.insert(noise.end(), nv.begin(), nv.end());
        if (csi && wv.size() == active.size()) weight.insert(weight.end(), wv.begin(), wv.end()); else weight.clear();
        const bool haveW = !weight.empty() && weight.size() == all.size();
        for (auto& pl : d.subframes[(size_t)si].plps) {
            PlpResult pr;
            pr.id = pl.id;
            FramePlp fp; fp.id = pl.id; fp.fecType = pl.fecType; fp.mod = pl.mod; fp.cod = pl.cod;
            pr.cfg = fp;
            BicmConfig bc = plpBicm(fp);
            Bicm bicm(bc);
            PlpAlloc a;
            a.start = pl.start; a.size = pl.size; a.dispersed = pl.type == 1; a.numSubslices = pl.numSubslices; a.subsliceInterval = pl.subsliceInterval;
            auto planeCells = [&](int first, int count, std::vector<cf32>& cells, double& nsum, std::vector<float>* wout = nullptr) {
                cells.resize((size_t)count);
                if (wout) wout->assign((size_t)count, 1.f);
                nsum = 0;
                for (int i = 0; i < count; i++) {
                    long idx = plpCellIndex(a, first + i);
                    cells[(size_t)i] = idx >= 0 && idx < (long)all.size() ? all[(size_t)idx] : cf32(0, 0);
                    nsum += idx >= 0 && idx < (long)noise.size() ? noise[(size_t)idx] : 0;
                    if (wout) (*wout)[(size_t)i] = idx >= 0 && idx < (long)weight.size() ? weight[(size_t)idx] : 0.f;
                }
            };
            if (bicm.ok() && pl.tiMode == 1 && pl.layer == 0 && a.size > 0) {
                pr.cti = true;
                pr.ctiRows = ctiRows(pl.ctiDepth, pl.tiExtendedInterleaving != 0);
                pr.ctiStartRow = pl.ctiStartRow; pr.ctiFecBlockStart = pl.ctiFecBlockStart;
                double nsum;
                planeCells(0, a.size, pr.cells, nsum, haveW ? &pr.cellW : nullptr);   // the weights go through the receiver's de-interleaver too
                pr.noise = (float)std::max(nsum / a.size, 0.002);
            } else if (bicm.ok() && pl.tiMode == 0 && pl.layer == 0 && a.size > 0) {
                const int cb = bicm.cells();
                const int first = std::min(std::max(pl.fecBlockStart, 0), a.size);   // the first complete FEC block starts here
                const int whole = (a.size - first) / cb;
                std::vector<std::vector<cf32>> blocks;
                std::vector<std::vector<float>> wblocks;
                double nsum = 0;
                for (int b = 0; b < whole; b++) {
                    blocks.emplace_back();
                    wblocks.emplace_back();
                    double ns;
                    planeCells(first + b * cb, cb, blocks.back(), ns, haveW ? &wblocks.back() : nullptr);
                    nsum += ns;
                }
                const float nz = (float)std::max(nsum / std::max<double>(1.0, (double)blocks.size() * cb), 0.002);
                if (!blocks.empty()) decodeBlocks(bicm, blocks, nz, pr, haveW ? &wblocks : nullptr);
                if (first > 0 || first + whole * cb < a.size) {   // blocks across the frame boundaries
                    pr.straddle = true;
                    pr.noise = nz;
                    double ns;
                    planeCells(0, first, pr.head, ns, haveW ? &pr.headW : nullptr);
                    planeCells(first + whole * cb, a.size - first - whole * cb, pr.tail, ns, haveW ? &pr.tailW : nullptr);
                }
            } else if (bicm.ok() && pl.tiMode == 2 && pl.layer == 0 && pl.htiInterSubframe == 0 && a.size > 0 && !pl.htiNumFecBlocks.empty()) {
                // hybrid time interleaver inside the subframe: the interleaving frame is a few TI blocks, each deinterleaved on its own
                const int cb = bicm.cells();
                const int nTi = pl.htiNumTiBlocks + 1, nFecIf = pl.htiNumFecBlocks[0] + 1;
                const int nFecMax = (pl.htiNumFecBlocksMax + 1 + nTi - 1) / nTi;   // per TI block: the columns of the twisted block interleaver
                if ((long)nFecIf * cb <= a.size) {
                    std::vector<std::vector<cf32>> blocks;
                    std::vector<std::vector<float>> wblocks;
                    double nsum = 0;
                    int first = 0;
                    for (int t = 0; t < nTi; t++) {
                        const int nFec = htiBlocksInTiBlock(nFecIf, nTi, t);
                        std::vector<cf32> ti;
                        std::vector<float> tw;
                        double ns;
                        planeCells(first, nFec * cb, ti, ns, haveW ? &tw : nullptr);
                        nsum += ns;
                        first += nFec * cb;
                        std::vector<cf32> de = htiDeinterleave(ti, cb, nFec, nFecMax, pl.htiCellInterleaver != 0);
                        std::vector<cf32> dw;
                        if (haveW) {   // the weights through the same de-interleaver (as the real parts of a cell vector)
                            std::vector<cf32> twc(tw.size());
                            for (size_t i = 0; i < tw.size(); i++) twc[i] = cf32(tw[i], 0.f);
                            dw = htiDeinterleave(twc, cb, nFec, nFecMax, pl.htiCellInterleaver != 0);
                        }
                        for (int f = 0; f < nFec && (size_t)(f + 1) * cb <= de.size(); f++) {
                            blocks.emplace_back(de.begin() + (size_t)f * cb, de.begin() + (size_t)(f + 1) * cb);
                            if (haveW && dw.size() == de.size()) { wblocks.emplace_back((size_t)cb); for (int i = 0; i < cb; i++) wblocks.back()[(size_t)i] = dw[(size_t)f * cb + i].real(); }
                        }
                    }
                    if (!blocks.empty()) decodeBlocks(bicm, blocks, (float)std::max(nsum / ((double)nFecIf * cb), 0.002), pr, haveW && wblocks.size() == blocks.size() ? &wblocks : nullptr);
                }
            }
            r.plps.push_back(std::move(pr));
        }
    }
    r.ok = true;
    return r;
}

} // namespace atsc3
} // namespace dect2
