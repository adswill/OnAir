// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/diag.h"
#include "dect2/atsc3/atsc3_preamble.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>

namespace dect2 {
namespace atsc3 {

namespace {

// The L1-Detail block interleaver across Np symbols (A/322 7.2.5.2): y[i * Lr + j] = x[j * Lc + i], for the first Lr * Lc cells.
void detailInterleave(const std::vector<cf32>& x, int np, std::vector<cf32>& y) {
    int total = (int)x.size(), lr = total / np;
    y = x;
    for (int i = 0; i < np; i++)
        for (int j = 0; j < lr; j++) y[i * lr + j] = x[j * np + i];
}

int creduced(int symbol, const L1Basic& l1) { return symbol == 0 ? 4 : l1.preambleReducedCarriers; }

} // namespace

std::vector<cf32> buildPreamble(const Bootstrap& bs, L1Basic& l1, const L1Detail& detail, unsigned fillSeed, int* leftoverOut, const std::vector<cf32>* payload) {
    PreambleParams pp;
    if (!preambleParams(bs.preambleStructure, pp)) {
        DIAG_DEBUG(613983, "No ATSC 3.0 preamble can be built for preamble structure %d, so no preamble is produced.", (int)bs.preambleStructure);
        return {};
    }
    int mode = l1.l1DetailFecType + 1;
    int sizeBytes = l1DetailSizeBytes(l1, detail);
    l1.l1DetailSizeBytes = sizeBytes;
    auto bits = packL1Detail(l1, detail, sizeBytes);
    auto detailCells = encodeL1Detail(bits, mode);
    l1.l1DetailTotalCells = (int)detailCells.size();
    auto basicCells = encodeL1Basic(l1, pp.l1BasicMode);
    int np = l1.preambleNumSymbols + 1;
    std::vector<cf32> inter;
    detailInterleave(detailCells, np, inter);
    std::mt19937 rng(fillSeed);
    std::vector<cf32> out;
    size_t pos = 0;
    for (int s = 0; s < np; s++) {
        int nd = (int)preambleDataCarriers(pp, creduced(s, l1)).size();
        std::vector<cf32> cells(nd);
        int used = 0;
        if (s == 0) { for (auto& c : basicCells) cells[used++] = c; }
        while (used < nd && pos < inter.size()) cells[used++] = inter[pos++];
        if (s == np - 1 && leftoverOut) *leftoverOut = nd - used;
        size_t pi = 0;
        for (; used < nd; used++) {
            if (s == np - 1 && payload && pi < payload->size()) cells[used] = (*payload)[pi++];
            else cells[used] = cf32((rng() & 1) ? -0.7071f : 0.7071f, (rng() & 1) ? -0.7071f : 0.7071f);
        }
        auto sym = modulatePreambleSymbol(pp, creduced(s, l1), s, cells);
        out.insert(out.end(), sym.begin(), sym.end());
    }
    if (pos < inter.size()) {   // the signalling does not fit into the Preamble
        DIAG_DEBUG(461419, "The ATSC 3.0 L1 signalling needs %zu cells but the preamble holds only %zu, so no preamble is produced.", inter.size(), pos);
        return {};
    }
    return out;
}

int preambleLeftoverCells(const Bootstrap& bs, const L1Basic& l1) {
    PreambleParams pp;
    if (!preambleParams(bs.preambleStructure, pp)) return 0;
    int total = 0;
    for (int s = 0; s <= l1.preambleNumSymbols; s++) total += (int)preambleDataCarriers(pp, creduced(s, l1)).size();
    return total - l1BasicCells(pp.l1BasicMode) - l1.l1DetailTotalCells;
}

PreambleResult decodePreamble(const cf32* x, size_t n, const Bootstrap& bs) {
    PreambleResult r;
    PreambleParams pp;
    if (!preambleParams(bs.preambleStructure, pp)) {
        DIAG_DEBUG_EVERY(780056, 5.0, "The bootstrap announces ATSC 3.0 preamble structure %d, which is not defined, so the preamble is not decoded.", bs.preambleStructure);
        return r;
    }
    const size_t symLen = (size_t)pp.fftSize + pp.guard;
    if (n < symLen) {
        DIAG_DEBUG_EVERY(468976, 5.0, "Only %zu samples follow the ATSC 3.0 bootstrap, fewer than one preamble symbol (%zu), so the preamble is not decoded.", n, symLen);
        return r;
    }
    std::vector<cf32> cells0;
    std::vector<float> w0;
    float nv = 0;
    // 4.0 (docs/SENSITIVITY-4.0-X3.md): the channel state of each cell in the L1 soft bits (DECT2_ATSC3_NOCSI=1: one noise figure, as 3.9)
    const char* noCsiEnv = getenv("DECT2_ATSC3_NOCSI");
    const bool csi = !(noCsiEnv && *noCsiEnv && *noCsiEnv != '0');
    if (!demodulatePreambleSymbol(pp, 4, 0, x, cells0, &nv, csi ? &w0 : nullptr)) {
        DIAG_DEBUG_EVERY(412825, 5.0, "The first ATSC 3.0 preamble symbol could not be demodulated, so the frame is not decoded.");
        return r;
    }
    r.noiseVar = nv;
    int nb = l1BasicCells(pp.l1BasicMode);
    if ((int)cells0.size() < nb) {
        DIAG_DEBUG_EVERY(463501, 5.0, "The first ATSC 3.0 preamble symbol has %zu cells but L1-Basic needs %d, so the frame is not decoded.", cells0.size(), nb);
        return r;
    }
    const bool haveW0 = csi && w0.size() == cells0.size();
    float noise = std::max(nv, 0.02f);
    r.basicOk = decodeL1Basic(cells0.data(), nb, noise, pp.l1BasicMode, r.basic, &r.ldpcIterationsBasic, haveW0 ? w0.data() : nullptr);
    if (!r.basicOk) return r;
    const L1Basic& l1 = r.basic;
    int np = l1.preambleNumSymbols + 1;
    r.numSymbols = np;
    r.samples = symLen * np;
    if (n < r.samples) {
        DIAG_DEBUG_EVERY(330055, 5.0, "The ATSC 3.0 preamble needs %zu samples but only %zu are present, so the frame is not decoded.", r.samples, n);
        return r;
    }
    // all Preamble symbols; the free cells (all but L1-Basic) in order are the interleaved L1-Detail cells
    std::vector<cf32> free(cells0.begin() + nb, cells0.end());
    std::vector<float> freeW;
    if (haveW0) freeW.assign(w0.begin() + nb, w0.end());
    bool haveW = haveW0;
    for (int s = 1; s < np; s++) {
        std::vector<cf32> c;
        std::vector<float> cw;
        if (!demodulatePreambleSymbol(pp, creduced(s, l1), s, x + (size_t)s * symLen, c, nullptr, haveW ? &cw : nullptr)) {
            DIAG_DEBUG_EVERY(911282, 5.0, "ATSC 3.0 preamble symbol %d could not be demodulated, so the frame is not decoded.", s);
            return r;
        }
        free.insert(free.end(), c.begin(), c.end());
        if (haveW && cw.size() == c.size()) freeW.insert(freeW.end(), cw.begin(), cw.end()); else haveW = false;
    }
    int total = l1.l1DetailTotalCells;
    if (total <= 0 || total > (int)free.size()) {
        DIAG_DEBUG_EVERY(998766, 5.0, "L1-Basic announces %d L1-Detail cells but the preamble has %zu free cells, so the frame is not decoded.", total, free.size());
        return r;
    }
    // undo the block interleaver: x[j * Lc + i] = y[i * Lr + j]
    std::vector<cf32> y(free.begin(), free.begin() + total), d(y);
    int lr = total / np;
    for (int i = 0; i < np; i++)
        for (int j = 0; j < lr; j++) d[j * np + i] = y[i * lr + j];
    std::vector<float> dw;
    if (haveW && freeW.size() >= (size_t)total) {
        dw.assign(freeW.begin(), freeW.begin() + total);
        for (int i = 0; i < np; i++)
            for (int j = 0; j < lr; j++) dw[(size_t)(j * np + i)] = freeW[(size_t)(i * lr + j)];
    }
    r.leftover.assign(d.begin() + 0, d.begin() + 0);   // filled below
    {   // the cells after the L1-Detail cells (before the block interleaver: interleaving only touches the first total cells)
        size_t after = (size_t)total;
        if (after < free.size()) r.leftover.assign(free.begin() + after, free.end());
        r.leftoverNoise = noise;
    }
    int mode = l1.l1DetailFecType + 1;
    int need = l1DetailCells(l1.l1DetailSizeBytes, mode);
    if (need <= 0 || need > total) {
        DIAG_DEBUG_EVERY(628735, 5.0, "L1-Basic announces an L1-Detail of %d bytes needing %d cells but only %d are allotted, so the frame is not decoded.", l1.l1DetailSizeBytes, need, total);
        return r;
    }
    std::vector<uint8_t> bits;
    if (!decodeL1Detail(d.data(), need, noise, mode, l1.l1DetailSizeBytes, bits, nullptr, dw.size() >= (size_t)need ? dw.data() : nullptr)) return r;
    r.detailOk = unpackL1Detail(l1, bits, r.detail);
    return r;
}

} // namespace atsc3
} // namespace dect2
