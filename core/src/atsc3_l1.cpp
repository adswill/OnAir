#include "dect2/atsc3_l1.h"
#include "atsc3_constellations.h"
#include "atsc3_ldpc.h"
#include "dect2/t2fec.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace atsc3 {

namespace {

// ---- bit packing
struct BitWriter {
    std::vector<uint8_t> b;
    void put(int value, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((value >> i) & 1); }
};
struct BitReader {
    const std::vector<uint8_t>& b;
    size_t pos = 0;
    explicit BitReader(const std::vector<uint8_t>& v) : b(v) {}
    int get(int n) { int v = 0; for (int i = 0; i < n; i++) v = (v << 1) | b[pos++]; return v; }
};

// ---- scrambler of A/322 5.2.3: x^16 + x^13 + x^12 + x^11 + x^7 + x^6 + x^3 + x + 1, initial state 0xF180.
// NOTE: the standard's text gives the first output values, but the bit order of the register could not be confirmed against a real
// transmitter; this is the straightforward serial reading (output taken from the last stage).
std::vector<uint8_t> scramblerBits(int n) {
    std::vector<uint8_t> out(n);
    int r[16];   // r[0] = X1 ... r[15] = X16
    for (int i = 0; i < 16; i++) r[i] = (0xF180 >> (15 - i)) & 1;
    for (int i = 0; i < n; i++) {
        out[i] = r[15];
        int fb = r[15] ^ r[12] ^ r[11] ^ r[10] ^ r[6] ^ r[5] ^ r[2] ^ r[0];
        for (int j = 15; j > 0; j--) r[j] = r[j - 1];
        r[0] = fb;
    }
    return out;
}

// ---- the protection chain, common to L1-Basic and L1-Detail (A/322 6.5.2)
struct Prot {
    int rate15, kldpc, nparity, ngInfo;      // LDPC code: rate, information bits, parity bits, information groups of 360 bits
    bool parityInterleave;                   // Type B codes: the parity interleaver of 6.5.2.6
    const int* shortOrder;                   // Table 6.20 row
    const int* parityOrder;                  // Tables 6.21 and 6.22 row: order for the groups ngInfo .. 44
    int eta;                                 // bits per cell
    long punctureA_num, punctureA_den, punctureB;   // Table 6.24
    long repC_num, repC_den, repD;           // Table 6.23 (Nrepeat = 2 floor(C Nouter) + D), repetition only when repOn
    bool repOn;
    Nuc nuc;
};

const int kSoBasic[9] = {4, 1, 5, 2, 8, 6, 0, 7, 3};
const int kSo1[9] = {7, 8, 5, 4, 1, 2, 6, 3, 0}, kSo2[9] = {6, 1, 7, 8, 0, 2, 4, 3, 5};
const int kSo3[18] = {0, 12, 15, 13, 2, 5, 7, 9, 8, 6, 16, 10, 14, 1, 17, 11, 4, 3};
const int kSo4[18] = {0, 15, 5, 16, 17, 1, 6, 13, 11, 4, 7, 12, 8, 14, 2, 3, 9, 10};
const int kSo5[18] = {2, 4, 5, 17, 9, 7, 1, 6, 15, 8, 10, 14, 16, 0, 11, 13, 12, 3};
const int kSo7[18] = {15, 7, 8, 11, 5, 10, 16, 4, 12, 3, 0, 6, 9, 1, 14, 17, 2, 13};

const int kPoBasic[36] = {20, 23, 25, 32, 38, 41, 18, 9, 10, 11, 31, 24, 14, 15, 26, 40, 33, 19, 28, 34, 16, 39, 27, 30, 21, 44, 43, 35, 42, 36, 12, 13, 29, 22, 37, 17};
const int kPo1[36] = {16, 22, 27, 30, 37, 44, 20, 23, 25, 32, 38, 41, 9, 10, 17, 18, 21, 33, 35, 14, 28, 12, 15, 19, 11, 24, 29, 34, 36, 13, 40, 43, 31, 26, 39, 42};
const int kPo2[36] = {9, 31, 23, 10, 11, 25, 43, 29, 36, 16, 27, 34, 26, 18, 37, 15, 13, 17, 35, 21, 20, 24, 44, 12, 22, 40, 19, 32, 38, 41, 30, 33, 14, 28, 39, 42};
const int kPo3[27] = {19, 37, 30, 42, 23, 44, 27, 40, 21, 34, 25, 32, 29, 24, 26, 35, 39, 20, 18, 43, 31, 36, 38, 22, 33, 28, 41};
const int kPo4[27] = {20, 35, 42, 39, 26, 23, 30, 18, 28, 37, 32, 27, 44, 43, 41, 40, 38, 36, 34, 33, 31, 29, 25, 24, 22, 21, 19};
const int kPo5[27] = {19, 37, 33, 26, 40, 43, 22, 29, 24, 35, 44, 31, 27, 20, 21, 39, 25, 42, 34, 18, 32, 38, 23, 30, 28, 36, 41};
const int kPo7[27] = {44, 23, 29, 33, 24, 28, 21, 27, 42, 18, 22, 31, 32, 37, 43, 30, 25, 35, 20, 34, 39, 36, 19, 41, 40, 26, 38};

Prot protFor(bool detail, int mode) {
    Prot p{};
    if (!detail) {
        static const int eta[5] = {2, 2, 2, 4, 6};
        static const long b[5] = {9360, 11460, 12360, 12292, 12350};
        static const Nuc nuc[5] = {Nuc::Qpsk, Nuc::Qpsk, Nuc::Qpsk, Nuc::Nuc16_8, Nuc::Nuc64_9};
        p = Prot{3, 3240, 12960, 9, false, kSoBasic, kPoBasic, eta[mode - 1], 0, 1, b[mode - 1], 0, 1, 3672, mode == 1, nuc[mode - 1]};
        return p;
    }
    switch (mode) {
    case 1: return Prot{3, 3240, 12960, 9, false, kSo1, kPo1, 2, 7, 2, 0, 61, 16, -508, true, Nuc::Qpsk};
    case 2: return Prot{3, 3240, 12960, 9, false, kSo2, kPo2, 2, 2, 1, 6036, 0, 1, 0, false, Nuc::Qpsk};
    case 3: return Prot{6, 6480, 9720, 18, true, kSo3, kPo3, 2, 11, 16, 4653, 0, 1, 0, false, Nuc::Qpsk};
    case 4: return Prot{6, 6480, 9720, 18, true, kSo4, kPo4, 4, 29, 32, 3200, 0, 1, 0, false, Nuc::Nuc16_8};
    case 5: return Prot{6, 6480, 9720, 18, true, kSo5, kPo5, 6, 3, 4, 4284, 0, 1, 0, false, Nuc::Nuc64_9};
    case 6: return Prot{6, 6480, 9720, 18, true, kSo4, kPo4, 8, 11, 16, 4900, 0, 1, 0, false, Nuc::Nuc256_9};
    default: return Prot{6, 6480, 9720, 18, true, kSo7, kPo7, 8, 49, 256, 8246, 0, 1, 0, false, Nuc::Nuc256_13};
    }
}

struct Sizes { int nouter, nrep, npunc, nfec, total, rows; };
Sizes sizesFor(const Prot& p, int ksig) {
    Sizes z;
    z.nouter = ksig + 168;
    long npTemp = p.punctureA_num * (p.kldpc - z.nouter) / p.punctureA_den + p.punctureB;
    long nfecTemp = z.nouter + p.nparity - npTemp;
    z.nfec = (int)((nfecTemp + p.eta - 1) / p.eta * p.eta);
    z.npunc = (int)(npTemp - (z.nfec - nfecTemp));
    z.nrep = p.repOn ? (int)(2 * (p.repC_num * z.nouter / p.repC_den) + p.repD) : 0;
    z.total = z.nfec + z.nrep;
    z.rows = z.total / p.eta;
    return z;
}

// positions of the Nouter transmitted bits among the Kldpc LDPC information bits (zero padding of 6.5.2.4)
std::vector<int> infoPositions(const Prot& p, int nouter) {
    std::vector<uint8_t> pad(p.kldpc, 0);
    int npad = (p.kldpc - nouter) / 360;
    for (int j = 0; j < npad; j++) std::fill(pad.begin() + 360 * p.shortOrder[j], pad.begin() + 360 * (p.shortOrder[j] + 1), 1);
    int part = p.kldpc - nouter - 360 * npad;
    if (npad < p.ngInfo) std::fill(pad.begin() + 360 * p.shortOrder[npad], pad.begin() + 360 * p.shortOrder[npad] + part, 1);
    std::vector<int> pos;
    for (int i = 0; i < p.kldpc; i++) if (!pad[i]) pos.push_back(i);
    return pos;
}

// Encodes ksig information bits: scrambling, BCH, LDPC, parity permutation, repetition, puncturing, then the block interleaver, bit
// demultiplexer and the constellation. Returns the cells.
std::vector<cf32> encodeBlock(const Prot& p, std::vector<uint8_t> bits) {
    const int ksig = (int)bits.size();
    Sizes z = sizesFor(p, ksig);
    auto scr = scramblerBits(ksig);
    for (int i = 0; i < ksig; i++) bits[i] ^= scr[i];
    static const BchCode bch(true, 12);
    bch.encode(bits, ksig);
    auto pos = infoPositions(p, z.nouter);
    std::vector<uint8_t> cw(p.kldpc, 0);
    for (int i = 0; i < z.nouter; i++) cw[pos[i]] = bits[i];
    ldpc16200(p.rate15).encode(cw);
    if (p.parityInterleave) {   // u(K + 360 t + s) = c(K + 27 s + t)
        std::vector<uint8_t> u(cw);
        for (int s = 0; s < 360; s++)
            for (int t = 0; t < 27; t++) u[p.kldpc + 360 * t + s] = cw[p.kldpc + 27 * s + t];
        cw.swap(u);
    }
    {   // group-wise permutation of the parity groups
        std::vector<uint8_t> y(cw);
        for (int j = p.ngInfo; j < 45; j++) {
            int src = p.parityOrder[j - p.ngInfo];
            std::copy(cw.begin() + 360 * src, cw.begin() + 360 * (src + 1), y.begin() + 360 * j);
        }
        cw.swap(y);
    }
    std::vector<uint8_t> word;
    for (int i = 0; i < z.nouter; i++) word.push_back(cw[pos[i]]);
    for (int i = 0; i < z.nrep; i++) word.push_back(cw[p.kldpc + i]);
    for (int i = 0; i < p.nparity - z.npunc; i++) word.push_back(cw[p.kldpc + i]);
    if ((int)word.size() != z.total) return {};
    const auto& cons = signallingConstellation(p.nuc);
    std::vector<cf32> cells(z.rows);
    for (int i = 0; i < z.rows; i++) {
        int label = 0;
        for (int j = 0; j < p.eta; j++) label = (label << 1) | word[((i + j) % p.eta) * z.rows + i];   // y_j = b_{(i+j) mod eta}(i)
        cells[i] = cons[label];
    }
    return cells;
}

// The reverse. Returns the ksig information bits (after descrambling) or false when LDPC or BCH fail.
bool decodeBlock(const Prot& p, const cf32* cells, float noiseVar, int ksig, std::vector<uint8_t>& out, int* iters) {
    Sizes z = sizesFor(p, ksig);
    const auto& cons = signallingConstellation(p.nuc);
    const int M = 1 << p.eta;
    std::vector<float> word(z.total, 0.f);
    float nv = std::max(noiseVar, 1e-4f);
    for (int i = 0; i < z.rows; i++) {
        float best0[8], best1[8];
        for (int j = 0; j < p.eta; j++) best0[j] = best1[j] = 1e30f;
        for (int lab = 0; lab < M; lab++) {
            float d = std::norm(cells[i] - cons[lab]);
            for (int j = 0; j < p.eta; j++) {
                if ((lab >> (p.eta - 1 - j)) & 1) best1[j] = std::min(best1[j], d); else best0[j] = std::min(best0[j], d);
            }
        }
        for (int j = 0; j < p.eta; j++) word[((i + j) % p.eta) * z.rows + i] = (best1[j] - best0[j]) / nv;
    }
    auto pos = infoPositions(p, z.nouter);
    std::vector<float> llr(16200, 0.f);
    for (int i = 0; i < p.kldpc; i++) llr[i] = 30.f;   // zero padding bits are known zeros
    for (int i = 0; i < z.nouter; i++) llr[pos[i]] = word[i];
    std::vector<float> par(p.nparity, 0.f);
    for (int i = 0; i < z.nrep; i++) par[i] += word[z.nouter + i];
    for (int i = 0; i < p.nparity - z.npunc; i++) par[i] += word[z.nouter + z.nrep + i];
    // undo the group-wise permutation (X_pi(j) = Y_j) and the parity interleaver
    std::vector<float> x(p.nparity, 0.f);
    for (int j = p.ngInfo; j < 45; j++) {
        int src = p.parityOrder[j - p.ngInfo];
        std::copy(par.begin() + 360 * (j - p.ngInfo), par.begin() + 360 * (j - p.ngInfo + 1), x.begin() + 360 * (src - p.ngInfo));
    }
    if (p.parityInterleave) {
        std::vector<float> c(p.nparity);
        for (int s = 0; s < 360; s++)
            for (int t = 0; t < 27; t++) c[27 * s + t] = x[360 * t + s];
        x.swap(c);
    }
    for (int i = 0; i < p.nparity; i++) llr[p.kldpc + i] = x[i];
    std::vector<uint8_t> hard;
    if (!ldpc16200(p.rate15).decode(llr, 60, hard, iters)) return false;
    std::vector<uint8_t> bits(z.nouter);
    for (int i = 0; i < z.nouter; i++) bits[i] = hard[pos[i]];
    static const BchCode bch(true, 12);
    if (bch.decode(bits) < 0) return false;
    bits.resize(ksig);
    auto scr = scramblerBits(ksig);
    for (int i = 0; i < ksig; i++) bits[i] ^= scr[i];
    out.swap(bits);
    return true;
}

} // namespace

uint32_t l1Crc32(const uint8_t* bits, int n) {
    uint32_t reg = 0xFFFFFFFFu;
    const uint32_t poly = (1u << 21) | (1u << 16) | (1u << 11) | 1u;   // x^32 + x^21 + x^16 + x^11 + 1
    for (int i = 0; i < n; i++) {
        uint32_t fb = ((reg >> 31) & 1) ^ bits[i];
        reg <<= 1;
        if (fb) reg ^= poly;
    }
    return reg;
}

std::vector<uint8_t> packL1Basic(const L1Basic& l) {
    BitWriter w;
    w.put(l.version, 3); w.put(l.mimoScatteredPilotEncoding, 1); w.put(l.llsFlag, 1); w.put(l.timeInfoFlag, 2);
    w.put(l.returnChannelFlag, 1); w.put(l.paprReduction, 2); w.put(l.frameLengthMode, 1);
    if (l.frameLengthMode == 0) { w.put(l.frameLength, 10); w.put(l.excessSamplesPerSymbol, 13); }
    else { w.put(l.timeOffset, 16); w.put(l.additionalSamples, 7); }
    w.put(l.numSubframes, 8); w.put(l.preambleNumSymbols, 3); w.put(l.preambleReducedCarriers, 3);
    w.put(l.l1DetailContentTag, 2); w.put(l.l1DetailSizeBytes, 13); w.put(l.l1DetailFecType, 3);
    w.put(l.l1DetailAdditionalParityMode, 2); w.put(l.l1DetailTotalCells, 19);
    w.put(l.firstSubMimo, 1); w.put(l.firstSubMiso, 2); w.put(l.firstSubFftSize, 2); w.put(l.firstSubReducedCarriers, 3);
    w.put(l.firstSubGuardInterval, 4); w.put(l.firstSubNumOfdmSymbols, 11); w.put(l.firstSubScatteredPilotPattern, 5);
    w.put(l.firstSubScatteredPilotBoost, 3); w.put(l.firstSubSbsFirst, 1); w.put(l.firstSubSbsLast, 1);
    w.put(0, 24); w.put(0, 24);   // L1B_reserved, 48 bits
    uint32_t crc = l1Crc32(w.b.data(), (int)w.b.size());
    for (int i = 31; i >= 0; i--) w.b.push_back((crc >> i) & 1);
    return w.b;
}

bool unpackL1Basic(const std::vector<uint8_t>& bits, L1Basic& l) {
    if ((int)bits.size() != kL1BasicBits) return false;
    BitReader r(bits);
    l = L1Basic();
    l.version = r.get(3); l.mimoScatteredPilotEncoding = r.get(1); l.llsFlag = r.get(1); l.timeInfoFlag = r.get(2);
    l.returnChannelFlag = r.get(1); l.paprReduction = r.get(2); l.frameLengthMode = r.get(1);
    if (l.frameLengthMode == 0) { l.frameLength = r.get(10); l.excessSamplesPerSymbol = r.get(13); }
    else { l.timeOffset = r.get(16); l.additionalSamples = r.get(7); }
    l.numSubframes = r.get(8); l.preambleNumSymbols = r.get(3); l.preambleReducedCarriers = r.get(3);
    l.l1DetailContentTag = r.get(2); l.l1DetailSizeBytes = r.get(13); l.l1DetailFecType = r.get(3);
    l.l1DetailAdditionalParityMode = r.get(2); l.l1DetailTotalCells = r.get(19);
    l.firstSubMimo = r.get(1); l.firstSubMiso = r.get(2); l.firstSubFftSize = r.get(2); l.firstSubReducedCarriers = r.get(3);
    l.firstSubGuardInterval = r.get(4); l.firstSubNumOfdmSymbols = r.get(11); l.firstSubScatteredPilotPattern = r.get(5);
    l.firstSubScatteredPilotBoost = r.get(3); l.firstSubSbsFirst = r.get(1); l.firstSubSbsLast = r.get(1);
    r.get(24); r.get(24);
    uint32_t crc = (uint32_t)r.get(16) << 16;
    crc |= (uint32_t)r.get(16);
    l.crcOk = crc == l1Crc32(bits.data(), 168);
    return l.crcOk;
}

int l1BasicCells(int mode) {
    if (mode < 1 || mode > 5) return 0;
    return sizesFor(protFor(false, mode), kL1BasicBits).rows;
}

std::vector<cf32> encodeL1Basic(const L1Basic& l1, int mode) {
    if (mode < 1 || mode > 5) return {};
    return encodeBlock(protFor(false, mode), packL1Basic(l1));
}

bool decodeL1Basic(const cf32* cells, int nCells, float noiseVar, int mode, L1Basic& out, int* iters) {
    if (mode < 1 || mode > 5 || nCells < l1BasicCells(mode)) return false;
    std::vector<uint8_t> bits;
    if (!decodeBlock(protFor(false, mode), cells, noiseVar, kL1BasicBits, bits, iters)) return false;
    return unpackL1Basic(bits, out);
}

// ---- L1-Detail

namespace {

const int kKseg[7] = {2352, 3072, 6312, 6312, 6312, 6312, 6312};

struct Seg { int n, ksig; };
Seg segmentsFor(int sizeBytes, int mode) {
    int kex = sizeBytes * 8;
    int n = (kex + kKseg[mode - 1] - 1) / kKseg[mode - 1];
    int kpad = (kex + 8 * n - 1) / (8 * n) * 8 * n - kex;
    return {n, (kex + kpad) / n};
}

} // namespace

int l1DetailCells(int sizeBytes, int mode) {
    if (mode < 1 || mode > 7 || sizeBytes < 25) return 0;
    Seg sg = segmentsFor(sizeBytes, mode);
    return sg.n * sizesFor(protFor(true, mode), sg.ksig).rows;
}

std::vector<cf32> encodeL1Detail(const std::vector<uint8_t>& bits, int mode) {
    if (mode < 1 || mode > 7 || bits.size() % 8 || bits.size() < 200) return {};
    Prot p = protFor(true, mode);
    Seg sg = segmentsFor((int)bits.size() / 8, mode);
    std::vector<cf32> cells;
    for (int i = 0; i < sg.n; i++) {
        std::vector<uint8_t> part(sg.ksig, 0);   // consecutive pieces, the last one padded with zeros
        size_t from = (size_t)i * sg.ksig;
        for (int k = 0; k < sg.ksig && from + k < bits.size(); k++) part[k] = bits[from + k];
        auto c = encodeBlock(p, part);
        if (c.empty()) return {};
        cells.insert(cells.end(), c.begin(), c.end());
    }
    return cells;
}

bool decodeL1Detail(const cf32* cells, int nCells, float noiseVar, int mode, int sizeBytes, std::vector<uint8_t>& bits, int* iters) {
    if (mode < 1 || mode > 7 || sizeBytes < 25 || nCells < l1DetailCells(sizeBytes, mode)) return false;
    Prot p = protFor(true, mode);
    Seg sg = segmentsFor(sizeBytes, mode);
    int rows = sizesFor(p, sg.ksig).rows;
    bits.clear();
    for (int i = 0; i < sg.n; i++) {
        std::vector<uint8_t> part;
        if (!decodeBlock(p, cells + (size_t)i * rows, noiseVar, sg.ksig, part, iters)) return false;
        bits.insert(bits.end(), part.begin(), part.end());
    }
    bits.resize((size_t)sizeBytes * 8);
    return true;
}

// ---- L1-Detail syntax (A/322 Table 9.8)

int l1DetailSizeBytes(const L1Basic& b, const L1Detail& d) {
    int n = (int)packL1Detail(b, d, 0).size() / 8;
    return n;
}

std::vector<uint8_t> packL1Detail(const L1Basic& b, const L1Detail& d, int sizeBytes) {
    BitWriter w;
    w.put(d.version, 4);
    w.put((int)d.bondedBsid.size(), 3);
    for (int id : d.bondedBsid) { w.put(id, 16); w.put(0, 3); }
    if (b.timeInfoFlag != 0) {
        w.put((int)d.timeSec, 32); w.put(d.timeMsec, 10);
        if (b.timeInfoFlag != 1) {
            w.put(d.timeUsec, 10);
            if (b.timeInfoFlag != 2) w.put(d.timeNsec, 10);
        }
    }
    const int numRf = (int)d.bondedBsid.size();
    for (int i = 0; i <= b.numSubframes && i < (int)d.subframes.size(); i++) {
        const L1DetailSubframe& sf = d.subframes[i];
        if (i > 0) {
            w.put(sf.mimo, 1); w.put(sf.miso, 2); w.put(sf.fftSize, 2); w.put(sf.reducedCarriers, 3); w.put(sf.guardInterval, 4);
            w.put(sf.numOfdmSymbols, 11); w.put(sf.scatteredPilotPattern, 5); w.put(sf.scatteredPilotBoost, 3);
            w.put(sf.sbsFirst, 1); w.put(sf.sbsLast, 1);
        }
        if (b.numSubframes > 0) w.put(sf.subframeMultiplex, 1);
        w.put(sf.frequencyInterleaver, 1);
        bool sbs = i == 0 ? (b.firstSubSbsFirst || b.firstSubSbsLast) : (sf.sbsFirst || sf.sbsLast);
        if (sbs) w.put(sf.sbsNullCells, 13);
        w.put((int)sf.plps.size() - 1, 6);
        bool mimo = i == 0 ? b.firstSubMimo : sf.mimo;
        for (const L1DetailPlp& p : sf.plps) {
            w.put(p.id, 6); w.put(p.llsFlag, 1); w.put(p.layer, 2); w.put(p.start, 24); w.put(p.size, 24);
            w.put(p.scramblerType, 2); w.put(p.fecType, 4);
            if (p.fecType <= 5) { w.put(p.mod, 4); w.put(p.cod, 4); }
            w.put(p.tiMode, 2);
            if (p.tiMode == 0) w.put(p.fecBlockStart, 15);
            else if (p.tiMode == 1) w.put(p.ctiFecBlockStart, 22);
            if (numRf > 0) {
                w.put(p.numChannelBonded, 3);
                if (p.numChannelBonded > 0) {
                    w.put(p.channelBondingFormat, 2);
                    for (int k = 0; k <= p.numChannelBonded; k++) w.put(k < (int)p.bondedRfId.size() ? p.bondedRfId[k] : 0, 3);
                }
            }
            if (mimo) { w.put(p.mimoStreamCombining, 1); w.put(p.mimoIqInterleaving, 1); w.put(p.mimoPh, 1); }
            if (p.layer == 0) {
                w.put(p.type, 1);
                if (p.type == 1) { w.put(p.numSubslices, 14); w.put(p.subsliceInterval, 24); }
                if ((p.tiMode == 1 || p.tiMode == 2) && p.mod == 0) w.put(p.tiExtendedInterleaving, 1);
                if (p.tiMode == 1) { w.put(p.ctiDepth, 3); w.put(p.ctiStartRow, 11); }
                else if (p.tiMode == 2) {
                    w.put(p.htiInterSubframe, 1); w.put(p.htiNumTiBlocks, 4); w.put(p.htiNumFecBlocksMax, 12);
                    if (p.htiInterSubframe == 0) w.put(p.htiNumFecBlocks.empty() ? 0 : p.htiNumFecBlocks[0], 12);
                    else for (int k = 0; k <= p.htiNumTiBlocks; k++) w.put(k < (int)p.htiNumFecBlocks.size() ? p.htiNumFecBlocks[k] : 0, 12);
                    w.put(p.htiCellInterleaver, 1);
                }
            } else {
                w.put(p.ldmInjectionLevel, 5);
            }
        }
    }
    w.put(d.bsid, 16);
    int minBytes = ((int)w.b.size() + 32 + 7) / 8;
    int bytes = sizeBytes > minBytes ? sizeBytes : minBytes;
    if (bytes < 25) bytes = 25;
    while ((int)w.b.size() < bytes * 8 - 32) w.b.push_back(0);   // L1D_reserved
    uint32_t crc = l1Crc32(w.b.data(), (int)w.b.size());
    for (int i = 31; i >= 0; i--) w.b.push_back((crc >> i) & 1);
    return w.b;
}

bool unpackL1Detail(const L1Basic& b, const std::vector<uint8_t>& bits, L1Detail& d) {
    d = L1Detail();
    if (bits.size() < 25 * 8 || bits.size() % 8) return false;
    // the reader must never run off the end: check before each group of fields
    BitReader r(bits);
    const size_t limit = bits.size() - 32;
    auto need = [&](int n) { return r.pos + n <= limit; };
    if (!need(7)) return false;
    d.version = r.get(4);
    int numRf = r.get(3);
    for (int i = 0; i < numRf; i++) { if (!need(19)) return false; d.bondedBsid.push_back(r.get(16)); r.get(3); }
    if (b.timeInfoFlag != 0) {
        if (!need(42)) return false;
        d.timeSec = (uint32_t)r.get(16) << 16; d.timeSec |= (uint32_t)r.get(16);
        d.timeMsec = r.get(10);
        if (b.timeInfoFlag != 1) {
            if (!need(10)) return false;
            d.timeUsec = r.get(10);
            if (b.timeInfoFlag != 2) { if (!need(10)) return false; d.timeNsec = r.get(10); }
        }
    }
    for (int i = 0; i <= b.numSubframes; i++) {
        L1DetailSubframe sf;
        if (i > 0) {
            if (!need(33)) return false;
            sf.mimo = r.get(1); sf.miso = r.get(2); sf.fftSize = r.get(2); sf.reducedCarriers = r.get(3); sf.guardInterval = r.get(4);
            sf.numOfdmSymbols = r.get(11); sf.scatteredPilotPattern = r.get(5); sf.scatteredPilotBoost = r.get(3);
            sf.sbsFirst = r.get(1); sf.sbsLast = r.get(1);
        }
        if (!need(2)) return false;
        if (b.numSubframes > 0) sf.subframeMultiplex = r.get(1);
        sf.frequencyInterleaver = r.get(1);
        bool sbs = i == 0 ? (b.firstSubSbsFirst || b.firstSubSbsLast) : (sf.sbsFirst || sf.sbsLast);
        if (sbs) { if (!need(13)) return false; sf.sbsNullCells = r.get(13); }
        if (!need(6)) return false;
        int nplp = r.get(6) + 1;
        bool mimo = i == 0 ? b.firstSubMimo : sf.mimo;
        for (int j = 0; j < nplp; j++) {
            L1DetailPlp p;
            if (!need(63)) return false;
            p.id = r.get(6); p.llsFlag = r.get(1); p.layer = r.get(2); p.start = r.get(24); p.size = r.get(24);
            p.scramblerType = r.get(2); p.fecType = r.get(4);
            if (p.fecType <= 5) { if (!need(8)) return false; p.mod = r.get(4); p.cod = r.get(4); }
            if (!need(2)) return false;
            p.tiMode = r.get(2);
            if (p.tiMode == 0) { if (!need(15)) return false; p.fecBlockStart = r.get(15); }
            else if (p.tiMode == 1) { if (!need(22)) return false; p.ctiFecBlockStart = r.get(22); }
            if (numRf > 0) {
                if (!need(3)) return false;
                p.numChannelBonded = r.get(3);
                if (p.numChannelBonded > 0) {
                    if (!need(2 + 3 * (p.numChannelBonded + 1))) return false;
                    p.channelBondingFormat = r.get(2);
                    for (int k = 0; k <= p.numChannelBonded; k++) p.bondedRfId.push_back(r.get(3));
                }
            }
            if (mimo) { if (!need(3)) return false; p.mimoStreamCombining = r.get(1); p.mimoIqInterleaving = r.get(1); p.mimoPh = r.get(1); }
            if (p.layer == 0) {
                if (!need(1)) return false;
                p.type = r.get(1);
                if (p.type == 1) { if (!need(38)) return false; p.numSubslices = r.get(14); p.subsliceInterval = r.get(24); }
                if ((p.tiMode == 1 || p.tiMode == 2) && p.mod == 0) { if (!need(1)) return false; p.tiExtendedInterleaving = r.get(1); }
                if (p.tiMode == 1) { if (!need(14)) return false; p.ctiDepth = r.get(3); p.ctiStartRow = r.get(11); }
                else if (p.tiMode == 2) {
                    if (!need(17)) return false;
                    p.htiInterSubframe = r.get(1); p.htiNumTiBlocks = r.get(4); p.htiNumFecBlocksMax = r.get(12);
                    int cnt = p.htiInterSubframe == 0 ? 1 : p.htiNumTiBlocks + 1;
                    if (!need(12 * cnt + 1)) return false;
                    for (int k = 0; k < cnt; k++) p.htiNumFecBlocks.push_back(r.get(12));
                    p.htiCellInterleaver = r.get(1);
                }
            } else {
                if (!need(5)) return false;
                p.ldmInjectionLevel = r.get(5);
            }
            sf.plps.push_back(p);
        }
        d.subframes.push_back(sf);
    }
    if (!need(16)) return false;
    d.bsid = r.get(16);
    uint32_t want = l1Crc32(bits.data(), (int)limit), got = 0;
    for (size_t i = limit; i < bits.size(); i++) got = (got << 1) | bits[i];
    d.crcOk = want == got;
    return d.crcOk;
}

} // namespace atsc3
} // namespace dect2
