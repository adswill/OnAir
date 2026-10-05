#include "dect2/atsc3_preamble.h"
#include <algorithm>
#include <cmath>
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
    if (!preambleParams(bs.preambleStructure, pp)) return {};
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
    if (pos < inter.size()) return {};   // the signalling does not fit into the Preamble
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
    if (!preambleParams(bs.preambleStructure, pp)) return r;
    const size_t symLen = (size_t)pp.fftSize + pp.guard;
    if (n < symLen) return r;
    std::vector<cf32> cells0;
    float nv = 0;
    if (!demodulatePreambleSymbol(pp, 4, 0, x, cells0, &nv)) return r;
    r.noiseVar = nv;
    int nb = l1BasicCells(pp.l1BasicMode);
    if ((int)cells0.size() < nb) return r;
    float noise = std::max(nv, 0.02f);
    r.basicOk = decodeL1Basic(cells0.data(), nb, noise, pp.l1BasicMode, r.basic, &r.ldpcIterationsBasic);
    if (!r.basicOk) return r;
    const L1Basic& l1 = r.basic;
    int np = l1.preambleNumSymbols + 1;
    r.numSymbols = np;
    r.samples = symLen * np;
    if (n < r.samples) return r;
    // all Preamble symbols; the free cells (all but L1-Basic) in order are the interleaved L1-Detail cells
    std::vector<cf32> free(cells0.begin() + nb, cells0.end());
    for (int s = 1; s < np; s++) {
        std::vector<cf32> c;
        if (!demodulatePreambleSymbol(pp, creduced(s, l1), s, x + (size_t)s * symLen, c)) return r;
        free.insert(free.end(), c.begin(), c.end());
    }
    int total = l1.l1DetailTotalCells;
    if (total <= 0 || total > (int)free.size()) return r;
    // undo the block interleaver: x[j * Lc + i] = y[i * Lr + j]
    std::vector<cf32> y(free.begin(), free.begin() + total), d(y);
    int lr = total / np;
    for (int i = 0; i < np; i++)
        for (int j = 0; j < lr; j++) d[j * np + i] = y[i * lr + j];
    r.leftover.assign(d.begin() + 0, d.begin() + 0);   // filled below
    {   // the cells after the L1-Detail cells (before the block interleaver: interleaving only touches the first total cells)
        size_t after = (size_t)total;
        if (after < free.size()) r.leftover.assign(free.begin() + after, free.end());
        r.leftoverNoise = noise;
    }
    int mode = l1.l1DetailFecType + 1;
    int need = l1DetailCells(l1.l1DetailSizeBytes, mode);
    if (need <= 0 || need > total) return r;
    std::vector<uint8_t> bits;
    if (!decodeL1Detail(d.data(), need, noise, mode, l1.l1DetailSizeBytes, bits)) return r;
    r.detailOk = unpackL1Detail(l1, bits, r.detail);
    return r;
}

} // namespace atsc3
} // namespace dect2
