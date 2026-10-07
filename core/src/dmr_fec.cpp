// DMR error control codes (ETSI TS 102 361-1 annex B). The generator matrices, the interleaving rules and the trellis tables are the ones of
// the specification (tables B.2 to B.18, figure B.9); tests/test_dmr_fec.cpp checks them against the worked example of annex D and against
// bursts that were captured off the air and published with the ok-dmrlib test suite.
#include "dect2/dmr_fec.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {
namespace dmr {

// ---------------------------------------------------------------------------------------------------- bit helpers

void putBits(Bits& b, uint64_t v, int n) {
    for (int i = n - 1; i >= 0; i--) b.push_back((uint8_t)((v >> i) & 1));
}

uint64_t getBits(const Bits& b, size_t pos, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | (pos + i < b.size() ? (b[pos + i] & 1) : 0);
    return v;
}

void bytesToBits(const uint8_t* d, size_t n, Bits& out) {
    for (size_t i = 0; i < n; i++) putBits(out, d[i], 8);
}

void bitsToBytes(const Bits& b, size_t pos, size_t nbits, uint8_t* out) {
    for (size_t i = 0; i < nbits / 8; i++) out[i] = (uint8_t)getBits(b, pos + i * 8, 8);
}

int popcount32(uint32_t v) {
    int n = 0;
    while (v) { v &= v - 1; n++; }
    return n;
}

namespace {

// ---------------------------------------------------------------------------------------------------- generator matrices
// Each table lists the parity part of the systematic generator matrix of the specification, one row per information bit (the first
// information bit first), the first parity column as the top bit.

constexpr uint16_t kGolayRows[8] = {0x3DA, 0xD99, 0x6CD, 0x367, 0xDC6, 0xA97, 0x93E, 0x8EB};                       // table B.11 (20,8)
constexpr uint16_t kQrRows[7] = {0x04F, 0x11E, 0x1B7, 0x1E2, 0x1C9, 0x0E5, 0x073};                                 // table B.12 (16,7,6)
constexpr uint16_t kHam743Rows[4] = {0x5, 0x7, 0x6, 0x3};                                                          // table B.17
constexpr uint16_t kHam1393Rows[9] = {0xF, 0xE, 0x7, 0xA, 0x5, 0xB, 0xC, 0x6, 0x3};                                // table B.14
constexpr uint16_t kHam15113Rows[11] = {0x9, 0xD, 0xF, 0xE, 0x7, 0xA, 0x5, 0xB, 0xC, 0x6, 0x3};                    // table B.15
constexpr uint16_t kHam16114Rows[11] = {0x13, 0x1A, 0x1F, 0x1C, 0x0E, 0x15, 0x0B, 0x16, 0x19, 0x0D, 0x07};         // table B.16
constexpr uint16_t kHam17123Rows[12] = {0x1B, 0x1F, 0x1D, 0x1C, 0x0E, 0x07, 0x11, 0x1A, 0x0D, 0x14, 0x0A, 0x05};   // table B.13

unsigned encodeRows(const uint16_t* rows, int k, unsigned info) {
    unsigned p = 0;
    for (int i = 0; i < k; i++)
        if ((info >> (k - 1 - i)) & 1) p ^= rows[i];
    return p;
}

// Nearest-codeword decoding of a short code by exhaustive search (256 or 128 words): the minimum distance of these codes is 6 or 7, so
// anything within the correction radius is unique.
struct NearestCode {
    int k, p;
    std::vector<uint32_t> words;
    NearestCode(const uint16_t* rows, int kk, int pp) : k(kk), p(pp) {
        for (unsigned i = 0; i < (1u << k); i++) words.push_back((i << p) | encodeRows(rows, k, i));
    }
    int decode(unsigned w, int maxErr, unsigned& info) const {
        int best = 99; unsigned bi = 0;
        for (unsigned i = 0; i < words.size(); i++) {
            const int d = popcount32(words[i] ^ w);
            if (d < best) { best = d; bi = i; }
        }
        if (best > maxErr) return -1;
        info = bi;
        return best;
    }
};

const NearestCode& golayCode() { static const NearestCode c(kGolayRows, 8, 12); return c; }
const NearestCode& qrCode() { static const NearestCode c(kQrRows, 7, 9); return c; }
const NearestCode& ham743Code() { static const NearestCode c(kHam743Rows, 4, 3); return c; }

struct HammingTables {
    const uint16_t* rows; int k, p;
    std::vector<int8_t> pos;     // syndrome -> bit position (0 = first bit of the word), -1 none
    HammingTables(const uint16_t* r, int kk, int pp) : rows(r), k(kk), p(pp), pos(1u << pp, -1) {
        for (int j = 0; j < k + p; j++) {
            const unsigned s = j < k ? rows[j] : (1u << (p - 1 - (j - k)));
            pos[s] = (int8_t)j;
        }
    }
};

const HammingTables& hammingTables(HammingKind kind) {
    static const HammingTables t1393(kHam1393Rows, 9, 4), t15113(kHam15113Rows, 11, 4), t16114(kHam16114Rows, 11, 5), t17123(kHam17123Rows, 12, 5);
    switch (kind) {
    case kHam1393: return t1393;
    case kHam15113: return t15113;
    case kHam16114: return t16114;
    default: return t17123;
    }
}

} // namespace

unsigned golay2008Parity(unsigned info8) { return encodeRows(kGolayRows, 8, info8 & 0xFF); }
int golay2008Decode(unsigned word20, unsigned& info8) { return golayCode().decode(word20 & 0xFFFFF, 3, info8); }
unsigned qr1676Parity(unsigned info7) { return encodeRows(kQrRows, 7, info7 & 0x7F); }
int qr1676Decode(unsigned word16, unsigned& info7) { return qrCode().decode(word16 & 0xFFFF, 2, info7); }
unsigned hamming743Parity(unsigned info4) { return encodeRows(kHam743Rows, 4, info4 & 0xF); }
int hamming743Decode(unsigned word7, unsigned& info4) { return ham743Code().decode(word7 & 0x7F, 1, info4); }

int hammingInfoBits(HammingKind k) { return hammingTables(k).k; }
int hammingParityBits(HammingKind k) { return hammingTables(k).p; }

unsigned hammingParity(HammingKind kind, unsigned info) {
    const HammingTables& t = hammingTables(kind);
    return encodeRows(t.rows, t.k, info);
}

int hammingCorrect(HammingKind kind, unsigned& word) {
    const HammingTables& t = hammingTables(kind);
    const unsigned info = word >> t.p, par = word & ((1u << t.p) - 1);
    const unsigned syn = encodeRows(t.rows, t.k, info) ^ par;
    if (!syn) return 0;
    const int j = t.pos[syn];
    if (j < 0) return -1;
    word ^= 1u << (t.k + t.p - 1 - j);
    return 1;
}

// ---------------------------------------------------------------------------------------------------- Reed-Solomon (12,9)

namespace {

struct Gf256 {
    uint8_t ex[512], lg[256];
    Gf256() {
        unsigned x = 1;
        for (int i = 0; i < 255; i++) {
            ex[i] = (uint8_t)x; lg[x] = (uint8_t)i;
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;    // x^8 + x^4 + x^3 + x^2 + 1 (B.14)
        }
        for (int i = 255; i < 512; i++) ex[i] = ex[i - 255];
        lg[0] = 0;
    }
    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? ex[lg[a] + lg[b]] : 0; }
};

const Gf256& gf() { static const Gf256 g; return g; }

constexpr uint8_t kRsGen[3] = {0x0E, 0x38, 0x40};   // g(x) = x^3 + 0e x^2 + 38 x + 40 (B.10)

} // namespace

void rs129Parity(const uint8_t msg[9], uint8_t parity[3]) {
    const Gf256& g = gf();
    uint8_t r[3] = {0, 0, 0};
    for (int i = 0; i < 9; i++) {
        const uint8_t fb = msg[i] ^ r[0];
        r[0] = r[1] ^ g.mul(fb, kRsGen[0]);
        r[1] = r[2] ^ g.mul(fb, kRsGen[1]);
        r[2] = g.mul(fb, kRsGen[2]);
    }
    parity[0] = r[0]; parity[1] = r[1]; parity[2] = r[2];
}

int rs129Correct(uint8_t word[12]) {
    const Gf256& g = gf();
    uint8_t s[3] = {0, 0, 0};
    for (int j = 1; j <= 3; j++) {
        uint8_t acc = 0;
        for (int i = 0; i < 12; i++) acc = (uint8_t)(g.mul(acc, g.ex[j]) ^ word[i]);
        s[j - 1] = acc;
    }
    if (!s[0] && !s[1] && !s[2]) return 0;
    if (!s[0] || !s[1] || !s[2]) return -1;
    // one error of value e at power p: S1 = e a^p, S2 = e a^2p, S3 = e a^3p
    const int p = (g.lg[s[1]] - g.lg[s[0]] + 255) % 255;
    if (p > 11) return -1;
    const uint8_t e = g.ex[(g.lg[s[0]] - p + 255) % 255];
    if (g.mul(e, g.ex[(3 * p) % 255]) != s[2]) return -1;
    word[11 - p] ^= e;
    return 1;
}

// ---------------------------------------------------------------------------------------------------- checksums

uint16_t crcCcitt(const uint8_t* d, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)(d[i] << 8);
        for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return (uint16_t)~c;
}

uint8_t crc8Short(uint32_t bits28) {
    unsigned c = 0;
    for (int i = 27; i >= 0; i--) {
        c = (c << 1) | ((bits28 >> i) & 1);
        if (c & 0x100) c ^= 0x107;
    }
    for (int i = 0; i < 8; i++) {
        c <<= 1;
        if (c & 0x100) c ^= 0x107;
    }
    return (uint8_t)c;
}

uint16_t crc9(const uint8_t* d, size_t nbytes, unsigned dbsn) {
    unsigned c = 0;
    auto shift = [&](unsigned bit) {
        const unsigned top = (c >> 8) & 1;
        c = ((c << 1) | bit) & 0x1FF;
        if (top) c ^= 0x059;      // G9 = x^9 + x^6 + x^4 + x^3 + 1 (B.23)
    };
    for (size_t i = 0; i < nbytes; i++)
        for (int b = 7; b >= 0; b--) shift((d[i] >> b) & 1);
    for (int b = 6; b >= 0; b--) shift((dbsn >> b) & 1);
    for (int i = 0; i < 9; i++) shift(0);
    return (uint16_t)(c ^ 0x1FF);
}

uint32_t crc32Msg(const uint8_t* d, size_t n) {
    // the octets of each 16-bit word go in least significant octet first (B.8B); polynomial B.21, initial value 0, no inversion
    uint32_t c = 0;
    auto feed = [&](uint8_t b) {
        c ^= (uint32_t)b << 24;
        for (int i = 0; i < 8; i++) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
    };
    for (size_t i = 0; i + 1 < n; i += 2) { feed(d[i + 1]); feed(d[i]); }
    if (n & 1) feed(d[n - 1]);
    return c;
}

uint8_t checksum5(const uint8_t lc[9]) {
    unsigned s = 0;
    for (int i = 0; i < 9; i++) s += lc[i];
    return (uint8_t)(s % 31);
}

// ---------------------------------------------------------------------------------------------------- BPTC (196,96)

namespace {

inline int bptcIndex(int r, int c) { return 1 + r * 15 + c; }   // the sequential number of the encoder matrix (table B.2); R(3) has index 0

// Whether the 99 bits of the first nine rows carry I(95)..I(0): row 0 has three reserved bits in front.
void bptcInfoPositions(int out[96]) {
    int k = 0;
    for (int r = 0; r < 9; r++)
        for (int c = (r == 0 ? 3 : 0); c < 11; c++) out[k++] = r * 15 + c;
}

} // namespace

void bptc196Encode(const Bits& info96, Bits& tx196) {
    uint8_t m[13][15];
    std::memset(m, 0, sizeof m);
    int pos[96];
    bptcInfoPositions(pos);
    for (int i = 0; i < 96; i++) m[pos[i] / 15][pos[i] % 15] = info96[i] & 1;
    for (int r = 0; r < 9; r++) {
        unsigned w = 0;
        for (int c = 0; c < 11; c++) w = (w << 1) | m[r][c];
        const unsigned p = hammingParity(kHam15113, w);
        for (int j = 0; j < 4; j++) m[r][11 + j] = (p >> (3 - j)) & 1;
    }
    for (int c = 0; c < 15; c++) {
        unsigned w = 0;
        for (int r = 0; r < 9; r++) w = (w << 1) | m[r][c];
        const unsigned p = hammingParity(kHam1393, w);
        for (int j = 0; j < 4; j++) m[9 + j][c] = (p >> (3 - j)) & 1;
    }
    tx196.assign(196, 0);
    tx196[0] = 0;                                    // R(3) has index 0 and stays at position 0
    for (int r = 0; r < 13; r++)
        for (int c = 0; c < 15; c++) tx196[(bptcIndex(r, c) * 181) % 196] = m[r][c];
}

int bptc196Decode(const Bits& tx196, Bits& info96) {
    uint8_t m[13][15];
    for (int r = 0; r < 13; r++)
        for (int c = 0; c < 15; c++) m[r][c] = tx196[(bptcIndex(r, c) * 181) % 196] & 1;
    int corrected = 0;
    bool clean = false;
    for (int iter = 0; iter < 6 && !clean; iter++) {
        clean = true;
        for (int c = 0; c < 15; c++) {
            unsigned w = 0;
            for (int r = 0; r < 13; r++) w = (w << 1) | m[r][c];
            const int e = hammingCorrect(kHam1393, w);
            if (e > 0) {
                for (int r = 0; r < 13; r++) m[r][c] = (w >> (12 - r)) & 1;
                corrected++; clean = false;
            } else if (e < 0) clean = false;
        }
        for (int r = 0; r < 9; r++) {
            unsigned w = 0;
            for (int c = 0; c < 15; c++) w = (w << 1) | m[r][c];
            const int e = hammingCorrect(kHam15113, w);
            if (e > 0) {
                for (int c = 0; c < 15; c++) m[r][c] = (w >> (14 - c)) & 1;
                corrected++; clean = false;
            } else if (e < 0) clean = false;
        }
    }
    if (!clean) {
        // one last check, without correcting: the loop may have ended just after a successful pass
        clean = true;
        for (int c = 0; c < 15 && clean; c++) {
            unsigned w = 0;
            for (int r = 0; r < 13; r++) w = (w << 1) | m[r][c];
            if (hammingParity(kHam1393, w >> 4) != (w & 15)) clean = false;
        }
        for (int r = 0; r < 9 && clean; r++) {
            unsigned w = 0;
            for (int c = 0; c < 15; c++) w = (w << 1) | m[r][c];
            if (hammingParity(kHam15113, w >> 4) != (w & 15)) clean = false;
        }
        if (!clean) return -1;
    }
    int pos[96];
    bptcInfoPositions(pos);
    info96.assign(96, 0);
    for (int i = 0; i < 96; i++) info96[i] = m[pos[i] / 15][pos[i] % 15];
    return corrected;
}

// ---------------------------------------------------------------------------------------------------- embedded LC (128 bits in four fragments)

void embLcEncode(const uint8_t lc[9], Bits frag[4]) {
    uint8_t m[8][16];
    std::memset(m, 0, sizeof m);
    Bits lcBits;
    bytesToBits(lc, 9, lcBits);
    const unsigned cs = checksum5(lc);
    int k = 0;
    for (int r = 0; r < 7; r++) {
        const int nInfo = r < 2 ? 11 : 10;
        for (int c = 0; c < nInfo; c++) m[r][c] = lcBits[k++];
        if (r >= 2) m[r][10] = (cs >> (6 - r)) & 1;          // CS(4) in row 2 down to CS(0) in row 6
        unsigned w = 0;
        for (int c = 0; c < 11; c++) w = (w << 1) | m[r][c];
        const unsigned p = hammingParity(kHam16114, w);
        for (int j = 0; j < 5; j++) m[r][11 + j] = (p >> (4 - j)) & 1;
    }
    for (int c = 0; c < 16; c++) {
        unsigned x = 0;
        for (int r = 0; r < 7; r++) x ^= m[r][c];
        m[7][c] = (uint8_t)x;
    }
    for (int f = 0; f < 4; f++) {
        frag[f].assign(32, 0);
        for (int i = 0; i < 32; i++) {
            const int s = f * 32 + i;                        // the columns are read top to bottom, left to right
            frag[f][i] = m[s % 8][s / 8];
        }
    }
}

int embLcDecode(const Bits frag[4], uint8_t lc[9]) {
    uint8_t m[8][16];
    for (int f = 0; f < 4; f++)
        for (int i = 0; i < 32; i++) {
            const int s = f * 32 + i;
            m[s % 8][s / 8] = frag[f][i] & 1;
        }
    int corrected = 0;
    for (int iter = 0; iter < 3; iter++) {
        bool changed = false;
        for (int r = 0; r < 7; r++) {
            unsigned w = 0;
            for (int c = 0; c < 16; c++) w = (w << 1) | m[r][c];
            const int e = hammingCorrect(kHam16114, w);
            if (e > 0) {
                for (int c = 0; c < 16; c++) m[r][c] = (w >> (15 - c)) & 1;
                corrected++; changed = true;
            } else if (e < 0) return -1;
        }
        // column parity: a single wrong bit in a column that rows could not see (a parity bit in the bottom row) is fixed here
        for (int c = 0; c < 16; c++) {
            unsigned x = 0;
            for (int r = 0; r < 8; r++) x ^= m[r][c];
            if (x) {
                // the Hamming rows are right at this point, so the fault is in the parity row
                m[7][c] ^= 1; corrected++; changed = true;
            }
        }
        if (!changed) break;
    }
    Bits bits;
    for (int r = 0; r < 7; r++) {
        const int nInfo = r < 2 ? 11 : 10;
        for (int c = 0; c < nInfo; c++) bits.push_back(m[r][c]);
    }
    bitsToBytes(bits, 0, 72, lc);
    unsigned cs = 0;
    for (int r = 2; r < 7; r++) cs = (cs << 1) | m[r][10];
    if (cs != checksum5(lc)) return -2;
    return corrected;
}

// ---------------------------------------------------------------------------------------------------- Short LC in the CACH

void shortLcEncode(uint32_t lc28, Bits piece[4]) {
    const uint64_t all = ((uint64_t)(lc28 & 0x0FFFFFFF) << 8) | crc8Short(lc28 & 0x0FFFFFFF);   // 36 bits: SLCO, data, CRC-8
    uint8_t m[4][17];
    std::memset(m, 0, sizeof m);
    for (int r = 0; r < 3; r++) {
        const unsigned info = (unsigned)((all >> (24 - 12 * r)) & 0xFFF);
        const unsigned p = hammingParity(kHam17123, info);
        for (int j = 0; j < 12; j++) m[r][j] = (info >> (11 - j)) & 1;
        for (int j = 0; j < 5; j++) m[r][12 + j] = (p >> (4 - j)) & 1;
    }
    for (int c = 0; c < 17; c++) m[3][c] = (uint8_t)(m[0][c] ^ m[1][c] ^ m[2][c]);
    for (int f = 0; f < 4; f++) {
        piece[f].assign(17, 0);
        for (int i = 0; i < 17; i++) {
            const int s = f * 17 + i;
            piece[f][i] = m[s % 4][s / 4];
        }
    }
}

int shortLcDecode(const Bits piece[4], uint32_t& lc28) {
    uint8_t m[4][17];
    for (int f = 0; f < 4; f++)
        for (int i = 0; i < 17; i++) {
            const int s = f * 17 + i;
            m[s % 4][s / 4] = piece[f][i] & 1;
        }
    int corrected = 0;
    uint64_t all = 0;
    for (int r = 0; r < 3; r++) {
        unsigned w = 0;
        for (int c = 0; c < 17; c++) w = (w << 1) | m[r][c];
        const int e = hammingCorrect(kHam17123, w);
        if (e < 0) return -1;
        corrected += e;
        all = (all << 12) | (w >> 5);
    }
    lc28 = (uint32_t)(all >> 8) & 0x0FFFFFFF;
    if ((all & 0xFF) != crc8Short(lc28)) return -2;
    return corrected;
}

// ---------------------------------------------------------------------------------------------------- CACH burst

namespace {
// The transmit order of the 24 CACH bits. The TACT (access type, channel, LCSS(1), LCSS(0), then the three Hamming parity bits) sits at transmit
// positions 0, 4, 8, 12, 14, 18 and 22; the 17 payload bits fill the other positions in order. The positions are those of two open-source
// implementations that work on real signals (MMDVM firmware DMRTX.cpp, OP25 dmr_const.h: cach_tact_bits and cach_payload_bits); the order
// within the TACT is table B.17 and the Hamming(7,4) matrix of the same table.
// Entries are (kind, index): kind 0 AT, 1 TC, 2 LS1, 3 LS0, 4 parity bit H(index) with H(2) the first one on the air, 5 payload bit P(index) with P(16) first.
struct CachSlot { uint8_t kind, idx; };
constexpr CachSlot kCachOrder[24] = {
    {0, 0}, {5, 16}, {5, 15}, {5, 14}, {1, 0}, {5, 13}, {5, 12}, {5, 11}, {2, 0}, {5, 10}, {5, 9}, {5, 8},
    {3, 0}, {5, 7}, {4, 2}, {5, 6}, {5, 5}, {5, 4}, {4, 1}, {5, 3}, {5, 2}, {5, 1}, {4, 0}, {5, 0}};
} // namespace

void cachEncode(int at, int tc, int lcss, unsigned payload17, Bits& out24) {
    const unsigned info = (unsigned)((at & 1) << 3 | (tc & 1) << 2 | (lcss & 3));
    const unsigned par = hamming743Parity(info);
    out24.assign(24, 0);
    for (int i = 0; i < 24; i++) {
        const CachSlot s = kCachOrder[i];
        unsigned v = 0;
        switch (s.kind) {
        case 0: v = (info >> 3) & 1; break;
        case 1: v = (info >> 2) & 1; break;
        case 2: v = (info >> 1) & 1; break;
        case 3: v = info & 1; break;
        case 4: v = (par >> s.idx) & 1; break;
        default: v = (payload17 >> s.idx) & 1; break;
        }
        out24[i] = (uint8_t)v;
    }
}

int cachDecode(const Bits& in24, int& at, int& tc, int& lcss, unsigned& payload17) {
    unsigned info = 0, par = 0;
    payload17 = 0;
    for (int i = 0; i < 24; i++) {
        const CachSlot s = kCachOrder[i];
        const unsigned v = in24[i] & 1;
        switch (s.kind) {
        case 0: info |= v << 3; break;
        case 1: info |= v << 2; break;
        case 2: info |= v << 1; break;
        case 3: info |= v; break;
        case 4: par |= v << s.idx; break;
        default: payload17 |= v << s.idx; break;
        }
    }
    unsigned fixed = 0;
    const int e = hamming743Decode((info << 3) | par, fixed);
    if (e < 0) return -1;
    at = (fixed >> 3) & 1; tc = (fixed >> 2) & 1; lcss = fixed & 3;
    return e;
}

// ---------------------------------------------------------------------------------------------------- rate 3/4 trellis

namespace {

constexpr uint8_t kTrellisNext[8][8] = {   // table B.7: state, input tribit -> constellation point
    {0, 8, 4, 12, 2, 10, 6, 14}, {4, 12, 2, 10, 6, 14, 0, 8}, {1, 9, 5, 13, 3, 11, 7, 15}, {5, 13, 3, 11, 7, 15, 1, 9},
    {3, 11, 7, 15, 1, 9, 5, 13}, {7, 15, 1, 9, 5, 13, 3, 11}, {2, 10, 6, 14, 0, 8, 4, 12}, {6, 14, 0, 8, 4, 12, 2, 10}};
constexpr int8_t kConstellation[16][2] = {   // table B.8: point -> first and second dibit as 4FSK symbols
    {1, -1}, {-1, -1}, {3, -3}, {-3, -3}, {-3, -1}, {3, -1}, {-1, -3}, {1, -3}, {-3, 3}, {3, 3}, {-1, 1}, {1, 1}, {1, 3}, {-1, 3}, {3, 1}, {-3, 1}};

// table B.9: transmit position k carries encoder dibit f(k): four runs of 26, 24, 24 and 24 positions
int trellisInterleave(int k) {
    int base, j;
    if (k < 26) { base = 0; j = k; }
    else if (k < 50) { base = 2; j = k - 26; }
    else if (k < 74) { base = 4; j = k - 50; }
    else { base = 6; j = k - 74; }
    return base + 8 * (j / 2) + (j & 1);
}

} // namespace

void trellis34Encode(const uint8_t data[18], Bits& tx196) {
    Bits bits;
    bytesToBits(data, 18, bits);
    int sym[98];
    int state = 0;
    for (int i = 0; i < 49; i++) {
        const int t = i < 48 ? (int)getBits(bits, i * 3, 3) : 0;      // 48 tribits, then the flushing tribit 000
        const int pt = kTrellisNext[state][t];
        state = t;
        sym[2 * i] = kConstellation[pt][0];
        sym[2 * i + 1] = kConstellation[pt][1];
    }
    tx196.clear();
    for (int k = 0; k < 98; k++) putBits(tx196, symbolToDibit(sym[trellisInterleave(k)]), 2);
}

int trellis34Decode(const float symbols[98], uint8_t data[18]) {
    float enc[98];
    for (int k = 0; k < 98; k++) enc[trellisInterleave(k)] = symbols[k];
    float metric[8], nm[8];
    uint8_t from[49][8];
    for (int s = 0; s < 8; s++) metric[s] = s == 0 ? 0.f : 1e30f;
    for (int i = 0; i < 49; i++) {
        for (int t = 0; t < 8; t++) nm[t] = 1e30f;
        for (int s = 0; s < 8; s++) {
            if (metric[s] > 1e29f) continue;
            for (int t = 0; t < 8; t++) {
                if (i == 48 && t != 0) continue;                       // the last tribit is the zero flush
                const int pt = kTrellisNext[s][t];
                const float a = enc[2 * i] - kConstellation[pt][0], b = enc[2 * i + 1] - kConstellation[pt][1];
                const float m = metric[s] + a * a + b * b;
                if (m < nm[t]) { nm[t] = m; from[i][t] = (uint8_t)s; }
            }
        }
        for (int t = 0; t < 8; t++) metric[t] = nm[t];
    }
    if (metric[0] > 1e29f) return -1;
    int tri[49];
    int st = 0;
    for (int i = 48; i >= 0; i--) {
        tri[i] = st;
        st = from[i][st];
    }
    Bits bits;
    for (int i = 0; i < 48; i++) putBits(bits, (uint64_t)tri[i], 3);
    bitsToBytes(bits, 0, 144, data);
    // how many dibits the winning path disagrees with, as a measure of the damage
    Bits re;
    trellis34Encode(data, re);
    int bad = 0;
    for (int k = 0; k < 98; k++) {
        const float v = symbols[k];
        const int hard = v >= 2.f ? 3 : v >= 0.f ? 1 : v >= -2.f ? -1 : -3;
        if (symbolToDibit(hard) != (unsigned)((re[2 * k] << 1) | re[2 * k + 1])) bad++;
    }
    return bad;
}

// ---------------------------------------------------------------------------------------------------- rate 1

void rate1Encode(const uint8_t data[24], Bits& tx196) {
    Bits d;
    bytesToBits(data, 24, d);
    tx196.clear();
    tx196.insert(tx196.end(), d.begin(), d.begin() + 96);
    for (int i = 0; i < 4; i++) tx196.push_back(0);
    tx196.insert(tx196.end(), d.begin() + 96, d.end());
}

void rate1Decode(const Bits& tx196, uint8_t data[24], bool* padOk) {
    Bits d;
    d.insert(d.end(), tx196.begin(), tx196.begin() + 96);
    d.insert(d.end(), tx196.begin() + 100, tx196.begin() + 196);
    bitsToBytes(d, 0, 192, data);
    if (padOk) *padOk = !(tx196[96] | tx196[97] | tx196[98] | tx196[99]);
}

} // namespace dmr
} // namespace dect2
