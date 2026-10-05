#include "dect2/atsc3_frame.h"
#include "dect2/atsc3_bb.h"
#include <algorithm>
#include <cmath>

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
    if (prefix < 0) return {};
    SubframeParams sp = subframeFrom(l1, d);
    sp.freqInterleaver = fs.freqInterleaver; sp.sbsNullCells = fs.sbsNullCells;
    const long total = subframeTotalCells(sp, prefix);
    // PLP cells and positions: one after the other, starting after the cells of the Preamble
    std::vector<std::pair<PlpAlloc, std::vector<cf32>>> placed;
    long pos = prefix;
    for (size_t i = 0; i < plps.size(); i++) {
        BicmConfig bc = plpBicm(plps[i]);
        Bicm bicm(bc);
        if (!bicm.ok()) return {};
        std::vector<cf32> cells;
        for (auto& pk : plps[i].bbPackets) {
            if ((int)pk.size() * 8 != bicm.kPayload()) return {};
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
    if (pos > total) return {};
    if (freeOut) *freeOut = (int)(total - pos);
    auto all = multiplexSubframe(sp, prefix, placed);
    std::vector<cf32> leftover(all.begin(), all.begin() + prefix), active(all.begin() + prefix, all.end());
    int got = 0;
    auto pre = buildPreamble(fs.bs, l1, d, 1, &got, &leftover);
    if (pre.empty() || got != prefix) return {};
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

FrameResult decodeFrame(const cf32* x, size_t n, const Bootstrap& bs) {
    FrameResult r;
    r.preamble = decodePreamble(x, n, bs);
    if (!r.preamble.basicOk || !r.preamble.detailOk || r.preamble.detail.subframes.empty()) return r;
    const L1Basic& l1 = r.preamble.basic;
    const L1Detail& d = r.preamble.detail;
    SubframeParams s = subframeFrom(l1, d);
    r.subframe = s;
    if (s.fftSize == 0 || s.guard == 0) return r;
    const size_t symLen = (size_t)s.fftSize + s.guard;
    if (n < r.preamble.samples + symLen * s.numSymbols) return r;
    std::vector<cf32> active;
    std::vector<float> nv;
    if (!demodulateSubframe(s, x + r.preamble.samples, n - r.preamble.samples, active, &nv)) return r;
    // data cell indices of the first subframe start in the last Preamble symbol
    std::vector<cf32> all(r.preamble.leftover);
    std::vector<float> noise(all.size(), r.preamble.leftoverNoise);
    all.insert(all.end(), active.begin(), active.end());
    noise.insert(noise.end(), nv.begin(), nv.end());
    for (auto& pl : d.subframes[0].plps) {
        PlpResult pr;
        pr.id = pl.id;
        FramePlp fp; fp.fecType = pl.fecType; fp.mod = pl.mod; fp.cod = pl.cod;
        BicmConfig bc = plpBicm(fp);
        Bicm bicm(bc);
        PlpAlloc a;
        a.start = pl.start; a.size = pl.size; a.dispersed = pl.type == 1; a.numSubslices = pl.numSubslices; a.subsliceInterval = pl.subsliceInterval;
        if (bicm.ok() && pl.tiMode == 0 && pl.layer == 0 && a.size > 0) {
            const int cb = bicm.cells();
            pr.blocks = a.size / cb;
            for (int b = 0; b < pr.blocks; b++) {
                std::vector<cf32> cells(cb);
                double nsum = 0;
                for (int i = 0; i < cb; i++) {
                    long idx = plpCellIndex(a, b * cb + i);
                    cells[i] = idx < (long)all.size() ? all[idx] : cf32(0, 0);
                    nsum += idx < (long)noise.size() ? noise[idx] : 0;
                }
                std::vector<uint8_t> bits;
                bool ok = bicm.decode(cells.data(), (float)std::max(nsum / cb, 0.002), bits);
                pr.ok.push_back(ok);
                if (ok) {
                    bbScramble(bits);
                    pr.packets.push_back(bitsToBytes(bits));
                    pr.blocksOk++;
                }
            }
        }
        r.plps.push_back(std::move(pr));
    }
    r.ok = true;
    return r;
}

} // namespace atsc3
} // namespace dect2
