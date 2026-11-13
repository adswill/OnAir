// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DVB-C2 L1 signalling part 2 (EN 302 769 clause 8): the fields of table 18, the CRC-32, the segmentation into 16K LDPC blocks (8.4.2),
// their coding (8.4.3 - 8.4.4: DVB-T2's shortened and punctured L1-post code, 16QAM) and the L1 time interleaving (8.5).
#include "dect2/dvbc2/dvbc2.h"
#include "dect2/t2/t2l1.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace dvbc2 {

namespace {
struct BitWriter {
    std::vector<uint8_t>& b;
    void io(int64_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((uint8_t)((v >> i) & 1)); }
    void ios(int v, int n) { io((int64_t)(v & ((1 << n) - 1)), n); }   // two's complement
};
struct BitReader {
    const std::vector<uint8_t>& b;
    size_t pos = 0;
    bool fail = false;
    int64_t get(int n) {
        int64_t r = 0;
        for (int i = 0; i < n; i++) { if (pos >= b.size()) { fail = true; r <<= 1; continue; } r = (r << 1) | b[pos++]; }
        return r;
    }
    int gets(int n) { int64_t v = get(n); if (v & (1ll << (n - 1))) v -= 1ll << n; return (int)v; }
};
constexpr int kL1Mod = 2;   // 16QAM in t2l1's numbering
constexpr int kEta = 4;
} // namespace

void packL1Part2(const L1Part2& l1, std::vector<uint8_t>& bits) {
    bits.clear();
    BitWriter w{bits};
    const int gi = l1.guardInterval & 3;
    const int posBits = gi == 0 ? 13 : 14, offBits = gi == 0 ? 8 : 9;
    w.io(l1.networkId, 16); w.io(l1.systemId, 16); w.io(l1.startFrequency, 24); w.io(l1.bandwidth, 16);
    w.io(gi, 2); w.io(l1.frameLength, 10); w.io(l1.changeCounter, 8);
    w.io((int)l1.slices.size(), 8); w.io((int)l1.notches.size(), 4);
    for (const L1DataSlice& s : l1.slices) {
        w.io(s.id, 8); w.io(s.tunePos, posBits); w.ios(s.offsetLeft, offBits); w.ios(s.offsetRight, offBits);
        w.io(s.tiDepth, 2); w.io(s.type, 1);
        if (s.type == 1) w.io(s.fecHeaderType, 1);
        w.io(s.constConf, 1); w.io(s.leftNotch, 1); w.io((int)s.plps.size(), 8);
        for (const L1Plp& p : s.plps) {
            w.io(p.id, 8); w.io(p.bundled, 1); w.io(p.type, 2); w.io(p.payloadType, 5);
            if (p.type == 0 || p.type == 1) w.io(p.groupId, 8);
            if (s.type == 0) { w.io(p.start, 14); w.io(p.fecType, 1); w.io(p.mod, 3); w.io(p.cod, 3); }
            w.io(p.psiSiReprocessing, 1);
            if (!p.psiSiReprocessing) { w.io(p.tsId, 16); w.io(p.onId, 16); }
            w.io(p.reserved1, 8);
        }
        w.io(s.reserved2, 8);
    }
    for (const L1Notch& n : l1.notches) { w.io(n.start, posBits); w.io(n.width, offBits); w.io(n.reserved3, 8); }
    w.io(l1.reservedTone, 1); w.io(l1.ews, 1); w.io(l1.version, 4); w.io(l1.reserved4, 11);
}

bool unpackL1Part2(const std::vector<uint8_t>& bits, L1Part2& l1) {
    BitReader r{bits};
    l1 = L1Part2();
    l1.networkId = (int)r.get(16); l1.systemId = (int)r.get(16); l1.startFrequency = r.get(24); l1.bandwidth = (int)r.get(16);
    l1.guardInterval = (int)r.get(2); l1.frameLength = (int)r.get(10); l1.changeCounter = (int)r.get(8);
    const int nSlices = (int)r.get(8), nNotch = (int)r.get(4);
    const int gi = l1.guardInterval;
    const int posBits = gi == 0 ? 13 : 14, offBits = gi == 0 ? 8 : 9;
    for (int i = 0; i < nSlices && !r.fail; i++) {
        L1DataSlice s;
        s.id = (int)r.get(8); s.tunePos = (int)r.get(posBits); s.offsetLeft = r.gets(offBits); s.offsetRight = r.gets(offBits);
        s.tiDepth = (int)r.get(2); s.type = (int)r.get(1);
        if (s.type == 1) s.fecHeaderType = (int)r.get(1);
        s.constConf = (int)r.get(1); s.leftNotch = (int)r.get(1);
        const int nPlp = (int)r.get(8);
        for (int j = 0; j < nPlp && !r.fail; j++) {
            L1Plp p;
            p.id = (int)r.get(8); p.bundled = (int)r.get(1); p.type = (int)r.get(2); p.payloadType = (int)r.get(5);
            if (p.type == 0 || p.type == 1) p.groupId = (int)r.get(8);
            if (s.type == 0) { p.start = (int)r.get(14); p.fecType = (int)r.get(1); p.mod = (int)r.get(3); p.cod = (int)r.get(3); }
            p.psiSiReprocessing = (int)r.get(1);
            if (!p.psiSiReprocessing) { p.tsId = (int)r.get(16); p.onId = (int)r.get(16); }
            p.reserved1 = (int)r.get(8);
            s.plps.push_back(p);
        }
        s.reserved2 = (int)r.get(8);
        l1.slices.push_back(s);
    }
    for (int i = 0; i < nNotch && !r.fail; i++) {
        L1Notch n;
        n.start = (int)r.get(posBits); n.width = (int)r.get(offBits); n.reserved3 = (int)r.get(8);
        l1.notches.push_back(n);
    }
    l1.reservedTone = (int)r.get(1); l1.ews = (int)r.get(1); l1.version = (int)r.get(4); l1.reserved4 = (int)r.get(11);
    return !r.fail && (l1.guardInterval == 0 || l1.guardInterval == 1) && nSlices >= 1;
}

std::string l1Summary(const L1Part2& l1) {
    char b[256];
    snprintf(b, sizeof b, "network %u system %u start carrier %lld (%.3f MHz) bandwidth %d carriers (%.2f MHz) GI 1/%d %zu data slice%s %zu notch%s",
             (unsigned)l1.networkId, (unsigned)l1.systemId, (long long)l1.startFrequency, l1.startFrequency * kCarrierSpacingHz / 1e6,
             l1.bandwidth * dx(l1.guardInterval) + 1, (l1.bandwidth * dx(l1.guardInterval) + 1) * kCarrierSpacingHz / 1e6,
             l1.guardInterval == 1 ? 64 : 128, l1.slices.size(), l1.slices.size() == 1 ? "" : "s", l1.notches.size(), l1.notches.size() == 1 ? "" : "es");
    return b;
}

bool l1Geometry(const PreambleHeader& h, int& fecBlocks, int& preambleSymbols, int& ksig, int& cellsPerBlock) {
    const int kExPad = 2 * h.l1InfoSize + 32;
    if (h.l1InfoSize < 40 || kExPad > kMaxL1Bits + 32) return false;
    fecBlocks = (kExPad + kMaxL1InfoPerBlock - 1) / kMaxL1InfoPerBlock;
    ksig = (kExPad + fecBlocks - 1) / fecBlocks;
    int depth = fecBlocks;
    if (h.tiMode == 2) depth = 4; else if (h.tiMode == 3) depth = 8;
    if (depth < fecBlocks) return false;   // more blocks than the signalled interleaving depth: not described by the standard
    preambleSymbols = depth;
    const int nBits = l1BlockBits(ksig, kL1Mod, 2 * kEta * depth);
    if (!nBits) return false;
    cellsPerBlock = nBits / kEta;
    if (kPreambleHeaderCells + (fecBlocks * cellsPerBlock) / depth > kL1DataCells) return false;
    return true;
}

bool encodeL1(const L1Part2& l1, int tiMode, L1Coded& out) {
    out = L1Coded();
    std::vector<uint8_t> bits;
    packL1Part2(l1, bits);
    if (bits.size() & 1) bits.push_back(0);   // L1 block padding (8.3.1)
    if ((int)bits.size() > kMaxL1Bits) return false;
    const uint32_t crc = crc32Bits(bits, bits.size());
    for (int n = 31; n >= 0; n--) bits.push_back((uint8_t)((crc >> n) & 1));
    out.header.l1InfoSize = (int)(bits.size() - 32) / 2;
    out.header.tiMode = tiMode & 3;
    if (!l1Geometry(out.header, out.fecBlocks, out.preambleSymbols, out.ksig, out.cellsPerBlock)) return false;
    bits.resize((size_t)out.ksig * (size_t)out.fecBlocks, 0);   // L1 padding (8.3.3)
    const int depth = out.preambleSymbols;
    std::vector<cf32> stream;
    for (int b = 0; b < out.fecBlocks; b++) {
        std::vector<uint8_t> part(bits.begin() + (size_t)b * out.ksig, bits.begin() + (size_t)(b + 1) * out.ksig);
        auto cells = encodeL1Block(part, kL1Mod, 2 * kEta * depth);
        if ((int)cells.size() != out.cellsPerBlock) return false;
        stream.insert(stream.end(), cells.begin(), cells.end());
    }
    out.rows.assign((size_t)depth, {});
    if (tiMode == 0) {
        for (int b = 0; b < out.fecBlocks; b++) out.rows[(size_t)b].assign(stream.begin() + (size_t)b * out.cellsPerBlock, stream.begin() + (size_t)(b + 1) * out.cellsPerBlock);
        for (int b = out.fecBlocks; b < depth; b++) out.rows[(size_t)b].assign((size_t)out.cellsPerBlock, cf32(0, 0));
    } else {
        const int width = (int)stream.size() / depth;
        std::vector<std::pair<int, int>> order;
        tiWriteOrder(depth, width, order);
        for (auto& row : out.rows) row.assign((size_t)width, cf32(0, 0));
        for (size_t i = 0; i < order.size(); i++) out.rows[(size_t)order[i].first][(size_t)order[i].second] = stream[i];
    }
    return true;
}

L1DecodeResult decodeL1(const PreambleHeader& h, const std::vector<std::vector<cf32>>& rows, const std::vector<std::vector<float>>& n0, L1Part2& out) {
    L1DecodeResult r;
    int fecBlocks, depth, ksig, cellsPerBlock;
    if (!l1Geometry(h, fecBlocks, depth, ksig, cellsPerBlock)) return r;
    if ((int)rows.size() < depth || (int)n0.size() < depth) return r;
    const int width = fecBlocks * cellsPerBlock / depth;
    for (int i = 0; i < depth; i++) if ((int)rows[(size_t)i].size() < width || (int)n0[(size_t)i].size() < width) return r;
    // undo the time interleaving into the stream of blocks
    std::vector<cf32> stream((size_t)fecBlocks * cellsPerBlock);
    std::vector<float> sn0(stream.size());
    if (h.tiMode == 0) {
        for (int b = 0; b < fecBlocks; b++) for (int c = 0; c < cellsPerBlock; c++) { stream[(size_t)b * cellsPerBlock + c] = rows[(size_t)b][(size_t)c]; sn0[(size_t)b * cellsPerBlock + c] = n0[(size_t)b][(size_t)c]; }
    } else {
        std::vector<std::pair<int, int>> order;
        tiWriteOrder(depth, width, order);
        for (size_t i = 0; i < order.size(); i++) { stream[i] = rows[(size_t)order[i].first][(size_t)order[i].second]; sn0[i] = n0[(size_t)order[i].first][(size_t)order[i].second]; }
    }
    std::vector<uint8_t> bits;
    r.blocksOk = true;
    for (int b = 0; b < fecBlocks; b++) {
        std::vector<cf32> y(stream.begin() + (size_t)b * cellsPerBlock, stream.begin() + (size_t)(b + 1) * cellsPerBlock);
        std::vector<float> yn(sn0.begin() + (size_t)b * cellsPerBlock, sn0.begin() + (size_t)(b + 1) * cellsPerBlock);
        float mean = 0;
        for (float v : yn) mean += v;
        mean /= (float)yn.size();
        std::vector<uint8_t> part;
        L1Result lr = decodeL1Block(y, mean, ksig, kL1Mod, 2 * kEta * depth, part, &yn);
        r.ldpcIters += lr.ldpcIters;
        if (!lr.bchOk) r.blocksOk = false;
        if ((int)part.size() != ksig) return r;
        bits.insert(bits.end(), part.begin(), part.end());
    }
    const int kExPad = 2 * h.l1InfoSize + 32;
    if ((int)bits.size() < kExPad) return r;
    uint32_t want = 0;
    for (int i = 0; i < 32; i++) want = (want << 1) | bits[(size_t)(kExPad - 32 + i)];
    if (crc32Bits(bits, (size_t)(kExPad - 32)) != want) return r;
    std::vector<uint8_t> info(bits.begin(), bits.begin() + (kExPad - 32));
    if (!unpackL1Part2(info, out)) return r;
    out.crcOk = true;
    r.ok = true;
    return r;
}

} // namespace dvbc2
} // namespace dect2
