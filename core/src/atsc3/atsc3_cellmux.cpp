#include "dect2/common/stage_prof.h"
#include "dect2/atsc3/atsc3_cellmux.h"
#include "dect2/atsc3/atsc3_bb.h"
#include <algorithm>

namespace dect2 {
namespace atsc3 {

int plpCellIndex(const PlpAlloc& a, int k) {
    if (!a.dispersed || a.numSubslices <= 0) return a.start + k;
    int sub = (a.size + a.numSubslices - 1) / a.numSubslices;   // subslice size = ceil(Nplp / Nsubslices)
    return a.start + (k / sub) * a.subsliceInterval + k % sub;
}

long subframeTotalCells(const SubframeParams& s, int prefix) {
    long n = prefix;
    for (int l = 0; l < s.numSymbols; l++) n += subframeActiveCells(s, l);
    return n;
}

std::vector<cf32> multiplexSubframe(const SubframeParams& s, int prefix, const std::vector<std::pair<PlpAlloc, std::vector<cf32>>>& plps) {
    long total = 0;
    for (int l = 0; l < s.numSymbols; l++) total += subframeActiveCells(s, l);
    total += prefix;
    auto seq = bbScrambleSequence((int)total);
    std::vector<cf32> all(total);
    for (long i = 0; i < total; i++) all[i] = cf32(1.f - 2.f * seq[i], 0.f);   // dummy modulation values
    for (auto& p : plps) {
        for (int k = 0; k < p.first.size && k < (int)p.second.size(); k++) {
            long idx = plpCellIndex(p.first, k);
            if (idx >= 0 && idx < total) all[idx] = p.second[k];
        }
    }
    return all;
}

std::vector<cf32> extractPlpCells(const std::vector<cf32>& active, const PlpAlloc& a) {
    std::vector<cf32> out;
    out.reserve(a.size);
    for (int k = 0; k < a.size; k++) {
        long idx = plpCellIndex(a, k);
        out.push_back(idx >= 0 && idx < (long)active.size() ? active[idx] : cf32(0, 0));
    }
    return out;
}

std::vector<cf32> modulateSubframe(const SubframeParams& s, const std::vector<cf32>& active) {
    std::vector<cf32> out;
    size_t pos = 0;
    for (int l = 0; l < s.numSymbols; l++) {
        int nd = subframeDataCells(s, l);
        int nulls = isBoundarySymbol(s, l) ? s.sbsNullCells : 0;
        std::vector<cf32> cells(nd, cf32(0, 0));
        // null cells: the lowest floor(N/2) and the highest ceil(N/2) data carriers; the active cells lie between them
        for (int i = nulls / 2; i < nd - (nulls - nulls / 2) && pos < active.size(); i++) cells[i] = active[pos++];
        auto sym = modulateSubframeSymbol(s, l, cells);
        out.insert(out.end(), sym.begin(), sym.end());
    }
    return out;
}

bool demodulateSubframe(const SubframeParams& s, const cf32* x, size_t n, std::vector<cf32>& active, std::vector<float>* noiseVar, std::vector<float>* weights) {
    const size_t symLen = (size_t)s.fftSize + s.guard;
    if (n < symLen * s.numSymbols) return false;
    active.clear();
    // the channel estimate of every symbol is averaged with those of its neighbours (a few symbols are far shorter than the channel changes)
    std::vector<SymbolRx> rx((size_t)s.numSymbols);
    DECT2_STAGE_VAR(stFft, "a3.ofdm.fft+est");
    for (int l = 0; l < s.numSymbols; l++)
        if (!receiveSymbol(s, l, x + symLen * l, rx[(size_t)l])) return false;
    const int half = 0;   // averaging over 2 symbols on each side made no difference on a real capture (-33 dB error vector at the decoder), so it is off
    stFft.stop();
    DECT2_STAGE("a3.ofdm.equalise");
    std::vector<cf32> h, cells;
    std::vector<float> w;
    if (weights) weights->clear();
    for (int l = 0; l < s.numSymbols; l++) {
        smoothChannel(rx, l, half, h);
        symbolCells(s, l, rx[(size_t)l], h, cells, weights ? &w : nullptr);
        int nd = (int)cells.size();
        int nulls = isBoundarySymbol(s, l) ? s.sbsNullCells : 0;
        for (int i = nulls / 2; i < nd - (nulls - nulls / 2); i++) {
            active.push_back(cells[(size_t)i]);
            if (noiseVar) noiseVar->push_back(rx[(size_t)l].nv);
            if (weights) weights->push_back(w[(size_t)i]);
        }
    }
    return true;
}

} // namespace atsc3
} // namespace dect2
