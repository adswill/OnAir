#include "dect2/t2l1.h"
#include "dect2/ldpc.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <vector>

namespace dect2 {

namespace {

// ------------------------------------------------------------------ tables (EN 302 755 annex / gr-dvbt2)
const int kLdpc14S[9][13] = {
    {12, 6295, 9626, 304, 7695, 4839, 4936, 1660, 144, 11203, 5567, 6347, 12557},
    {12, 10691, 4988, 3859, 3734, 3071, 3494, 7687, 10313, 5964, 8069, 8296, 11090},
    {12, 10774, 3613, 5208, 11177, 7676, 3549, 8746, 6583, 7239, 12265, 2674, 4292},
    {12, 11869, 3708, 5981, 8718, 4908, 10650, 6805, 3334, 2627, 10461, 9285, 11120},
    {3, 7844, 3079, 10773, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {3, 3385, 10854, 5747, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {3, 1360, 12010, 12202, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {3, 6189, 4241, 2343, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {3, 9840, 12726, 4977, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
const int kLdpc12S[20][9] = {
    {8, 20, 712, 2386, 6354, 4061, 1062, 5045, 5158}, {8, 21, 2543, 5748, 4822, 2348, 3089, 6328, 5876},
    {8, 22, 926, 5701, 269, 3693, 2438, 3190, 3507},  {8, 23, 2802, 4520, 3577, 5324, 1091, 4667, 4449},
    {8, 24, 5140, 2003, 1263, 4742, 6497, 1185, 6202}, {3, 0, 4046, 6934, 0, 0, 0, 0, 0},
    {3, 1, 2855, 66, 0, 0, 0, 0, 0},                  {3, 2, 6694, 212, 0, 0, 0, 0, 0},
    {3, 3, 3439, 1158, 0, 0, 0, 0, 0},                {3, 4, 3850, 4422, 0, 0, 0, 0, 0},
    {3, 5, 5924, 290, 0, 0, 0, 0, 0},                 {3, 6, 1467, 4049, 0, 0, 0, 0, 0},
    {3, 7, 7820, 2242, 0, 0, 0, 0, 0},                {3, 8, 4606, 3080, 0, 0, 0, 0, 0},
    {3, 9, 4633, 7877, 0, 0, 0, 0, 0},                {3, 10, 3884, 6868, 0, 0, 0, 0, 0},
    {3, 11, 8935, 4996, 0, 0, 0, 0, 0},               {3, 12, 3028, 764, 0, 0, 0, 0, 0},
    {3, 13, 5988, 1057, 0, 0, 0, 0, 0},               {3, 14, 7411, 3450, 0, 0, 0, 0, 0}};
const int kPrePuncture[36] = {27, 13, 29, 32, 5, 0, 11, 21, 33, 20, 25, 28, 18, 35, 8, 3, 9, 31,
                              22, 24, 7, 14, 17, 4, 2, 26, 16, 34, 19, 10, 12, 23, 1, 6, 30, 15};
const int kPostPaddingBQ[20] = {18, 17, 16, 15, 14, 13, 12, 11, 4, 10, 9, 8, 3, 2, 7, 6, 5, 1, 19, 0};
const int kPostPadding16[20] = {18, 17, 16, 15, 14, 13, 12, 11, 4, 10, 9, 8, 7, 3, 2, 1, 6, 5, 19, 0};
const int kPostPadding64[20] = {18, 17, 16, 4, 15, 14, 13, 12, 3, 11, 10, 9, 2, 8, 7, 1, 6, 5, 19, 0};
const int kPostPunctBQ[25] = {6, 4, 18, 9, 13, 8, 15, 20, 5, 17, 2, 24, 10, 22, 12, 3, 16, 23, 1, 14, 0, 21, 19, 7, 11};
const int kPostPunct16[25] = {6, 4, 13, 9, 18, 8, 15, 20, 5, 17, 2, 22, 24, 7, 12, 1, 16, 23, 14, 0, 21, 10, 19, 11, 3};
const int kPostPunct64[25] = {6, 15, 13, 10, 3, 17, 21, 8, 5, 19, 2, 23, 16, 24, 7, 18, 1, 12, 20, 0, 4, 14, 9, 11, 22};
const int kMux16[8] = {7, 1, 3, 5, 2, 4, 6, 0};
const int kMux64[12] = {11, 8, 5, 2, 10, 7, 4, 1, 9, 6, 3, 0};

constexpr int KBCH14 = 3072, NBCH14 = 3240, KBCH12 = 7032, NBCH12 = 7200, NBCH_PARITY = 168, FRAME_SHORT = 16200;
constexpr float kBig = 1e4f;

const LdpcCode& ldpcPre() {
    static LdpcCode c = [] {
        std::vector<std::vector<int>> rows;
        for (auto& r : kLdpc14S) rows.emplace_back(r + 1, r + 1 + r[0]);
        return LdpcCode(NBCH14, FRAME_SHORT, rows);
    }();
    return c;
}
const LdpcCode& ldpcPost() {
    static LdpcCode c = [] {
        std::vector<std::vector<int>> rows;
        for (auto& r : kLdpc12S) rows.emplace_back(r + 1, r + 1 + r[0]);
        return LdpcCode(NBCH12, FRAME_SHORT, rows);
    }();
    return c;
}

// ------------------------------------------------------------------ BCH (t = 12, short frames)
struct BchPoly {
    unsigned int w[6];
    BchPoly() {
        const int p[12][15] = {
            {1, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1}, {1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 1},
            {1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 1}, {1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 0, 1, 0, 1},
            {1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 1}, {1, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1},
            {1, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 1, 1}, {1, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1},
            {1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 1}, {1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1},
            {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1}, {1, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 0, 0, 1, 1}};
        std::vector<int> acc(p[0], p[0] + 15);
        for (int k = 1; k < 12; k++) {
            std::vector<int> out(acc.size() + 15, 0);
            for (size_t i = 0; i < acc.size(); i++)
                for (int j = 0; j < 15; j++)
                    if (acc[i] && p[k][j]) out[i + j]++;
            int mx = 0;
            for (size_t i = 0; i < out.size(); i++) { out[i] &= 1; if (out[i]) mx = (int)i; }
            out.resize(mx + 1);
            acc = out;
        }
        acc.resize(192, 0);
        for (int i = 0; i < 6; i++) {
            unsigned int t = 0x80000000u;
            w[i] = 0;
            for (int j = 0; j < 32; j++) { if (acc[i * 32 + j]) w[i] |= t; t >>= 1; }
        }
    }
};
const BchPoly& bchPoly() { static BchPoly p; return p; }

inline void reg6Shift(unsigned int* sr) {
    sr[5] = (sr[5] >> 1) | (sr[4] << 31);
    sr[4] = (sr[4] >> 1) | (sr[3] << 31);
    sr[3] = (sr[3] >> 1) | (sr[2] << 31);
    sr[2] = (sr[2] >> 1) | (sr[1] << 31);
    sr[1] = (sr[1] >> 1) | (sr[0] << 31);
    sr[0] = (sr[0] >> 1);
}

// Appends 168 BCH parity bits after the first k message bits.
void bchEncode(std::vector<uint8_t>& bits, int k) {
    const BchPoly& g = bchPoly();
    unsigned int sh[6] = {};
    for (int j = 0; j < k; j++) {
        int b = bits[j] ^ ((sh[5] & 0x01000000) ? 1 : 0);
        reg6Shift(sh);
        if (b) for (int i = 0; i < 6; i++) sh[i] ^= g.w[i];
    }
    bits.resize(k + NBCH_PARITY);
    for (int n = 0; n < NBCH_PARITY; n++) {
        bits[k + n] = (sh[5] & 0x01000000) ? 1 : 0;
        reg6Shift(sh);
    }
}

// true if the 168 parity bits match the message (i.e. the word is a valid BCH codeword)
bool bchCheck(const std::vector<uint8_t>& bits, int n) {
    int k = n - NBCH_PARITY;
    std::vector<uint8_t> m(bits.begin(), bits.begin() + k);
    bchEncode(m, k);
    for (int i = 0; i < NBCH_PARITY; i++) if (m[k + i] != bits[k + i]) return false;
    return true;
}

// ------------------------------------------------------------------ bit IO
struct BitWriter {
    std::vector<uint8_t>& b;
    template <class T> void io(T& v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back(((uint64_t)v >> i) & 1); }
};
struct BitReader {
    const std::vector<uint8_t>& b;
    size_t pos = 0;
    bool fail = false;
    template <class T> void io(T& v, int n) {
        uint64_t r = 0;
        for (int i = 0; i < n; i++) {
            if (pos >= b.size()) { fail = true; r <<= 1; continue; }
            r = (r << 1) | b[pos++];
        }
        v = (T)r;
    }
};

template <class IO> void walkPre(IO& io, L1Pre& p) {
    int s1 = p.s1, s2f1 = (p.s2 >> 1) & 7, s2f2 = p.s2 & 1;
    io.io(p.type, 8); io.io(p.bwtExt, 1); io.io(s1, 3); io.io(s2f1, 3); io.io(s2f2, 1);
    p.s1 = s1; p.s2 = (s2f1 << 1) | s2f2;
    io.io(p.repetition, 1); io.io(p.guardInterval, 3); io.io(p.papr, 4); io.io(p.l1Mod, 4); io.io(p.l1Cod, 2);
    io.io(p.l1Fec, 2); io.io(p.postSize, 18); io.io(p.postInfoSize, 18); io.io(p.pilotPattern, 4);
    io.io(p.txIdAvail, 8); io.io(p.cellId, 16); io.io(p.networkId, 16); io.io(p.systemId, 16);
    io.io(p.numFrames, 8); io.io(p.numDataSyms, 12); io.io(p.regen, 3); io.io(p.postExtension, 1);
    io.io(p.numRf, 3); io.io(p.curRf, 3); io.io(p.version, 4); io.io(p.postScrambled, 1); io.io(p.lite, 1);
    io.io(p.reserved, 4);
}

template <class IO> void walkPost(IO& io, const L1Pre& pre, bool fef, L1Post& p) {
    io.io(p.subSlices, 15); io.io(p.numPlp, 8); io.io(p.numAux, 4); io.io(p.auxRfu, 8);
    int nRf = std::max(1, pre.numRf);
    if ((int)p.rf.size() != nRf) p.rf.resize(nRf);
    for (auto& r : p.rf) { io.io(r.idx, 3); io.io(r.freq, 32); }
    if (fef) { io.io(p.fefType, 4); io.io(p.fefLength, 22); io.io(p.fefInterval, 8); }
    if ((int)p.plps.size() != p.numPlp) p.plps.resize(p.numPlp);
    for (auto& q : p.plps) {
        io.io(q.id, 8); io.io(q.type, 3); io.io(q.payloadType, 5); io.io(q.ff, 1); io.io(q.firstRf, 3);
        io.io(q.firstFrameIdx, 8); io.io(q.groupId, 8); io.io(q.cod, 3); io.io(q.mod, 3); io.io(q.rotation, 1);
        io.io(q.fecType, 2); io.io(q.numBlocksMax, 10); io.io(q.frameInterval, 8); io.io(q.timeIlLength, 8);
        io.io(q.timeIlType, 1); io.io(q.inBandA, 1); io.io(q.inBandB, 1); io.io(q.reserved1, 11);
        io.io(q.plpMode, 2); io.io(q.staticFlag, 1); io.io(q.staticPad, 1);
    }
    io.io(p.fefLengthMsb, 2); io.io(p.reserved2, 30);
    if ((int)p.auxConf.size() != p.numAux) p.auxConf.resize(p.numAux);
    for (auto& a : p.auxConf) io.io(a, 32);
    io.io(p.frameIdx, 8); io.io(p.subSliceInterval, 22); io.io(p.type2Start, 22); io.io(p.changeCounter, 8);
    io.io(p.startRfIdx, 3); io.io(p.dynReserved1, 8);
    if ((int)p.dyn.size() != p.numPlp) p.dyn.resize(p.numPlp);
    for (auto& d : p.dyn) { io.io(d.id, 8); io.io(d.start, 22); io.io(d.numBlocks, 10); io.io(d.reserved2, 8); }
    io.io(p.dynReserved3, 8);
    if ((int)p.auxDyn.size() != p.numAux) p.auxDyn.resize(p.numAux);
    for (auto& a : p.auxDyn) io.io(a, 48);
}

// ------------------------------------------------------------------ L1-post helpers
int etaMod(int mod) { return mod == 0 ? 1 : mod == 1 ? 2 : mod == 2 ? 4 : mod == 3 ? 6 : 1; }

const int* postPadding(int mod) { return mod == 2 ? kPostPadding16 : mod == 3 ? kPostPadding64 : kPostPaddingBQ; }
const int* postPuncture(int mod) { return mod == 2 ? kPostPunct16 : mod == 3 ? kPostPunct64 : kPostPunctBQ; }

// Marks the padded (non-information) positions of the 7032-bit L1-post BCH message.
void postPadMap(int ksig, int mod, std::vector<uint8_t>& pad) {
    const int* pp = postPadding(mod);
    pad.assign(KBCH12, 0);
    int m, last;
    if (ksig <= 360) { m = 19; last = 360 - ksig; }
    else { m = (KBCH12 - ksig) / 360; last = KBCH12 - ksig - 360 * m; }
    for (int n = 0; n < m; n++) {
        int idx = pp[n] * 360;
        int cnt = pp[n] == 19 ? 192 : 360;
        for (int w = 0; w < cnt; w++) pad[idx++] = 1;
    }
    int idx = pp[m] == 19 ? pp[m] * 360 + 192 - last : pp[m] * 360 + 360 - last;
    for (int w = 0; w < last; w++) pad[idx++] = 1;
}

struct PostDims { int ksig, npunc, npost; };
PostDims postDims(int ksig, int mod, int nP2) {
    int eta = etaMod(mod);
    int npuncTemp = (6 * (KBCH12 - ksig)) / 5;
    int npostTemp = ksig + NBCH_PARITY + 9000 - npuncTemp;
    int npost;
    if (nP2 == 1) npost = (int)std::ceil((float)npostTemp / (2.f * eta)) * 2 * eta;
    else npost = (int)std::ceil((float)npostTemp / ((float)eta * nP2)) * eta * nP2;
    return {ksig, npuncTemp - (npost - npostTemp), npost};
}

std::vector<uint8_t> postRandomizer() {
    std::vector<uint8_t> r(KBCH12);
    int sr = 0x4A80;
    for (int i = 0; i < KBCH12; i++) {
        int b = (sr ^ (sr >> 1)) & 1;
        r[i] = b;
        sr >>= 1;
        if (b) sr |= 0x4000;
    }
    return r;
}

} // namespace

// ------------------------------------------------------------------ public: bits
uint32_t crc32Bits(const std::vector<uint8_t>& bits, size_t n) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        uint32_t b = bits[i] ^ ((crc >> 31) & 1);
        crc <<= 1;
        if (b) crc ^= 0x04C11DB7u;
    }
    return crc;
}

void packL1Pre(const L1Pre& p, std::vector<uint8_t>& bits) {
    bits.clear();
    BitWriter w{bits};
    L1Pre c = p;
    walkPre(w, c);
}

bool unpackL1Pre(const std::vector<uint8_t>& bits, L1Pre& p) {
    BitReader r{bits};
    walkPre(r, p);
    return !r.fail;
}

void packL1Post(const L1Pre& pre, const L1Post& p, bool fef, std::vector<uint8_t>& bits) {
    bits.clear();
    BitWriter w{bits};
    L1Post c = p;
    walkPost(w, pre, fef, c);
}

bool unpackL1Post(const L1Pre& pre, const std::vector<uint8_t>& bits, bool fef, L1Post& p) {
    BitReader r{bits};
    p = L1Post();
    // NUM_PLP / NUM_AUX are read first so the vectors can be sized: peek them
    {
        BitReader pk{bits};
        int ss, np, na, rfu;
        pk.io(ss, 15); pk.io(np, 8); pk.io(na, 4); pk.io(rfu, 8);
        if (pk.fail) return false;
        p.numPlp = np; p.numAux = na;
    }
    walkPost(r, pre, fef, p);
    p.extBits = (int)bits.size() - (int)r.pos;
    return !r.fail;
}

// ------------------------------------------------------------------ constellations
int modBits(int m) { return m == 0 ? 1 : m == 1 ? 2 : m == 2 ? 4 : 6; }

cf32 mapCell(int mod, unsigned b) {
    switch (mod) {
    case 0: return cf32(b ? -1.f : 1.f, 0.f);
    case 1: {
        const float a = 0.70710678f;
        return cf32((b & 2) ? -a : a, (b & 1) ? -a : a);
    }
    case 2: {
        const float a = 1.f / std::sqrt(10.f);
        float re = ((b & 8) ? -1.f : 1.f) * ((b & 2) ? 1.f : 3.f);
        float im = ((b & 4) ? -1.f : 1.f) * ((b & 1) ? 1.f : 3.f);
        return cf32(re * a, im * a);
    }
    default: {
        const float a = 1.f / std::sqrt(42.f);
        static const float lv[4] = {7.f, 5.f, 1.f, 3.f};
        float re = ((b & 32) ? -1.f : 1.f) * lv[((b >> 2) & 2) | ((b >> 1) & 1)]; // {b3,b1}
        float im = ((b & 16) ? -1.f : 1.f) * lv[((b >> 1) & 2) | (b & 1)];        // {b2,b0}
        return cf32(re * a, im * a);
    }
    }
}

void demapCell(int mod, cf32 y, float n0, float* llr) {
    const int nb = modBits(mod);
    float best0[6], best1[6];
    for (int i = 0; i < nb; i++) best0[i] = best1[i] = 1e30f;
    for (unsigned s = 0; s < (1u << nb); s++) {
        cf32 d = y - mapCell(mod, s);
        float dist = std::norm(d);
        for (int i = 0; i < nb; i++) {
            int bit = (s >> (nb - 1 - i)) & 1;
            if (bit) best1[i] = std::min(best1[i], dist);
            else best0[i] = std::min(best0[i], dist);
        }
    }
    const float scale = 1.f / std::max(1e-6f, 2.f * n0);
    for (int i = 0; i < nb; i++) llr[i] = (best1[i] - best0[i]) * scale;
}

// ------------------------------------------------------------------ L1-pre
std::vector<cf32> encodeL1Pre(const L1Pre& pre) {
    std::vector<uint8_t> bits;
    packL1Pre(pre, bits);
    uint32_t crc = crc32Bits(bits, bits.size());
    for (int n = 31; n >= 0; n--) bits.push_back((crc >> n) & 1);
    bits.resize(KBCH14, 0);
    bchEncode(bits, KBCH14);                // 3240 bits
    std::vector<uint8_t> cw = bits;
    ldpcPre().encode(cw);                   // 16200 bits
    std::vector<uint8_t> punct(FRAME_SHORT - NBCH14, 0);
    for (int c = 0; c < 31; c++) for (int c2 = 0; c2 < 360; c2++) punct[c2 * 36 + kPrePuncture[c]] = 1;
    for (int c2 = 0; c2 < 328; c2++) punct[c2 * 36 + kPrePuncture[31]] = 1;
    std::vector<cf32> out;
    for (int i = 0; i < 200; i++) out.push_back(mapCell(0, cw[i]));
    for (int i = 0; i < NBCH_PARITY; i++) out.push_back(mapCell(0, cw[KBCH14 + i]));
    for (int i = 0; i < FRAME_SHORT - NBCH14; i++) if (!punct[i]) out.push_back(mapCell(0, cw[NBCH14 + i]));
    return out;
}

L1Result decodeL1Pre(const std::vector<cf32>& y, float n0, L1Pre& out) {
    L1Result r;
    if (y.size() < 1840) return r;
    std::vector<uint8_t> punct(FRAME_SHORT - NBCH14, 0);
    for (int c = 0; c < 31; c++) for (int c2 = 0; c2 < 360; c2++) punct[c2 * 36 + kPrePuncture[c]] = 1;
    for (int c2 = 0; c2 < 328; c2++) punct[c2 * 36 + kPrePuncture[31]] = 1;
    std::vector<float> llr(FRAME_SHORT, 0.f);
    const float sc = 2.f / std::max(1e-6f, n0);
    size_t k = 0;
    for (int i = 0; i < 200; i++) llr[i] = y[k++].real() * sc;
    for (int i = 200; i < KBCH14; i++) llr[i] = kBig;
    for (int i = 0; i < NBCH_PARITY; i++) llr[KBCH14 + i] = y[k++].real() * sc;
    for (int i = 0; i < FRAME_SHORT - NBCH14; i++) if (!punct[i]) llr[NBCH14 + i] = y[k++].real() * sc;
    std::vector<uint8_t> hard;
    bool ok = ldpcPre().decodeFast(llr, 100, hard, &r.ldpcIters, 0.8f);
    (void)ok;
    std::vector<uint8_t> info(hard.begin(), hard.begin() + 168);
    uint32_t want = 0;
    for (int i = 0; i < 32; i++) want = (want << 1) | hard[168 + i];
    r.crcOk = crc32Bits(info, 168) == want;
    r.bchOk = bchCheck(hard, NBCH14);
    if (r.crcOk && unpackL1Pre(info, out)) r.ok = true;
    return r;
}

// ------------------------------------------------------------------ L1-post
std::vector<cf32> encodeL1Post(L1Pre& pre, const L1Post& post, int nP2, bool fef) {
    std::vector<uint8_t> bits;
    packL1Post(pre, post, fef, bits);
    pre.postInfoSize = (int)bits.size();
    uint32_t crc = crc32Bits(bits, bits.size());
    for (int n = 31; n >= 0; n--) bits.push_back((crc >> n) & 1);
    const int ksig = (int)bits.size();
    const int mod = pre.l1Mod;
    if (pre.postScrambled) {
        auto rnd = postRandomizer();
        for (int n = 0; n < ksig; n++) bits[n] ^= rnd[n];
    }
    PostDims d = postDims(ksig, mod, nP2);
    pre.postSize = d.npost / etaMod(mod);

    std::vector<uint8_t> pad;
    postPadMap(ksig, mod, pad);
    std::vector<uint8_t> msg(KBCH12, 0);
    {
        int idx = 0;
        for (int n = 0; n < KBCH12; n++) if (!pad[n]) msg[n] = bits[idx++];
    }
    bchEncode(msg, KBCH12);                 // 7200 bits
    std::vector<uint8_t> cw = msg;
    ldpcPost().encode(cw);
    // puncturing
    std::vector<uint8_t> punct(FRAME_SHORT - NBCH12, 0);
    const int* pp = postPuncture(mod);
    for (int c = 0; c < d.npunc / 360; c++) for (int c2 = 0; c2 < 360; c2++) punct[c2 * 25 + pp[c]] = 1;
    for (int c2 = 0; c2 < d.npunc - (d.npunc / 360) * 360; c2++) punct[c2 * 25 + pp[d.npunc / 360]] = 1;
    std::vector<uint8_t> s; // bit stream after padding / puncture removal
    for (int n = 0; n < KBCH12; n++) if (!pad[n]) s.push_back(cw[n]);
    for (int n = 0; n < NBCH_PARITY; n++) s.push_back(cw[KBCH12 + n]);
    for (int n = 0; n < FRAME_SHORT - NBCH12; n++) if (!punct[n]) s.push_back(cw[NBCH12 + n]);
    // bit interleave (16QAM / 64QAM)
    std::vector<uint8_t> t = s;
    if (mod == 2 || mod == 3) {
        int cols = mod == 2 ? 8 : 12, rows = (int)s.size() / cols;
        for (int k = 0; k < rows; k++) for (int w = 0; w < cols; w++) t[k * cols + w] = s[rows * w + k];
    }
    std::vector<cf32> out;
    const int nb = modBits(mod);
    if (mod == 0) for (size_t i = 0; i < s.size(); i++) out.push_back(mapCell(0, s[i]));
    else if (mod == 1) for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(mapCell(1, (s[i] << 1) | s[i + 1]));
    else {
        const int* mux = mod == 2 ? kMux16 : kMux64;
        int grp = 2 * nb;
        for (size_t i = 0; i + grp <= s.size(); i += grp) {
            unsigned a = 0, b = 0;
            for (int e = 0; e < nb; e++) a = (a << 1) | t[i + mux[e]];
            for (int e = nb; e < grp; e++) b = (b << 1) | t[i + mux[e]];
            out.push_back(mapCell(mod, a));
            out.push_back(mapCell(mod, b));
        }
    }
    return out;
}

L1Result decodeL1Post(const std::vector<cf32>& y, float n0, const L1Pre& pre, int nP2, bool fef, L1Post& out) {
    L1Result r;
    const int mod = pre.l1Mod;
    if (mod < 0 || mod > 3 || pre.l1Cod != 0 || pre.l1Fec != 0) return r; // only rate 1/2, short FEC frames
    const int ksig = pre.postInfoSize + 32;
    if (ksig > KBCH12 || ksig < 33) return r;
    PostDims d = postDims(ksig, mod, nP2);
    const int nb = modBits(mod), eta = etaMod(mod);
    if ((int)y.size() < d.npost / eta || pre.postSize != d.npost / eta) return r;

    // demap to a bit-LLR stream in transmit-interleaved order
    std::vector<float> lt(d.npost, 0.f);
    float tmp[6];
    if (mod == 0 || mod == 1) {
        for (int c = 0; c < d.npost / eta; c++) {
            demapCell(mod, y[c], n0, tmp);
            for (int i = 0; i < nb; i++) lt[c * nb + i] = tmp[i];
        }
    } else {
        const int* mux = mod == 2 ? kMux16 : kMux64;
        int grp = 2 * nb;
        for (int g = 0, c = 0; g + grp <= d.npost; g += grp, c += 2) {
            for (int h = 0; h < 2; h++) {
                demapCell(mod, y[c + h], n0, tmp);
                for (int e = 0; e < nb; e++) lt[g + mux[h * nb + e]] = tmp[e];
            }
        }
    }
    std::vector<float> ls = lt;
    if (mod == 2 || mod == 3) {
        int cols = mod == 2 ? 8 : 12, rows = d.npost / cols;
        for (int k = 0; k < rows; k++) for (int w = 0; w < cols; w++) ls[rows * w + k] = lt[k * cols + w];
    }
    // rebuild the 16200-bit LDPC input
    std::vector<uint8_t> pad;
    postPadMap(ksig, mod, pad);
    std::vector<uint8_t> punct(FRAME_SHORT - NBCH12, 0);
    const int* pp = postPuncture(mod);
    for (int c = 0; c < d.npunc / 360; c++) for (int c2 = 0; c2 < 360; c2++) punct[c2 * 25 + pp[c]] = 1;
    for (int c2 = 0; c2 < d.npunc - (d.npunc / 360) * 360; c2++) punct[c2 * 25 + pp[d.npunc / 360]] = 1;
    std::vector<float> llr(FRAME_SHORT, 0.f);
    const float sc = 1.f; // demapCell already scales
    size_t k = 0;
    for (int n = 0; n < KBCH12; n++) llr[n] = pad[n] ? kBig : ls[k++] * sc;
    for (int n = 0; n < NBCH_PARITY; n++) llr[KBCH12 + n] = ls[k++] * sc;
    for (int n = 0; n < FRAME_SHORT - NBCH12; n++) if (!punct[n]) { if (k < ls.size()) llr[NBCH12 + n] = ls[k++] * sc; }
    std::vector<uint8_t> hard;
    ldpcPost().decodeFast(llr, 100, hard, &r.ldpcIters, 0.8f);
    r.bchOk = bchCheck(hard, NBCH12);
    std::vector<uint8_t> bits;
    for (int n = 0; n < KBCH12; n++) if (!pad[n]) bits.push_back(hard[n]);
    if ((int)bits.size() != ksig) return r;
    if (pre.postScrambled) {
        auto rnd = postRandomizer();
        for (int n = 0; n < ksig; n++) bits[n] ^= rnd[n];
    }
    uint32_t want = 0;
    for (int i = 0; i < 32; i++) want = (want << 1) | bits[ksig - 32 + i];
    r.crcOk = crc32Bits(bits, ksig - 32) == want;
    if (r.crcOk) {
        std::vector<uint8_t> info(bits.begin(), bits.begin() + (ksig - 32));
        r.ok = unpackL1Post(pre, info, fef, out);
        out.crcOk = r.ok;
    }
    return r;
}

// ------------------------------------------------------------------ P2 distribution
void p2Distribute(const std::vector<cf32>& stream, int nP2, int cP2, int nPre, int nPost, std::vector<std::vector<cf32>>& sym) {
    sym.assign(nP2, std::vector<cf32>(cP2, cf32(0, 0)));
    if (nP2 == 1) {
        for (int i = 0; i < cP2 && i < (int)stream.size(); i++) sym[0][i] = stream[i];
        return;
    }
    auto at = [&](int j) { return j < (int)stream.size() ? stream[j] : cf32(0, 0); };
    for (int n = 0; n < nP2; n++) {
        int idx = 0;
        for (int j = 0; j < nPre / nP2; j++) sym[n][idx++] = at(n + j * nP2);
        for (int j = 0; j < nPost / nP2; j++) sym[n][idx++] = at(nPre + n + j * nP2);
    }
    int rd = nPre + nPost;
    for (int n = 0; n < nP2; n++)
        for (int idx = (nPre + nPost) / nP2; idx < cP2; idx++) sym[n][idx] = at(rd++);
}

void p2Gather(const std::vector<std::vector<cf32>>& sym, int nP2, int cP2, int nPre, int nPost, std::vector<cf32>& stream) {
    stream.assign((size_t)nP2 * cP2, cf32(0, 0));
    if (nP2 == 1) { for (int i = 0; i < cP2; i++) stream[i] = sym[0][i]; return; }
    for (int n = 0; n < nP2; n++) {
        int idx = 0;
        for (int j = 0; j < nPre / nP2; j++) stream[n + j * nP2] = sym[n][idx++];
        for (int j = 0; j < nPost / nP2; j++) stream[nPre + n + j * nP2] = sym[n][idx++];
    }
    int rd = nPre + nPost;
    for (int n = 0; n < nP2; n++)
        for (int idx = (nPre + nPost) / nP2; idx < cP2; idx++) stream[rd++] = sym[n][idx];
}

} // namespace dect2
