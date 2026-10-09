// HD Radio channel coding (see hdr_fec.h). Adapted from nrsc5 (GPL-3.0, github.com/theori-io/nrsc5): the scrambler, the code polynomials,
// the puncture patterns and the interleaver equations come from src/decode.c, the check sums from src/frame.c and src/pids.c. The Viterbi
// decoder and the Reed-Solomon code are OnAir's own.
#include "dect2/hdr_fec.h"
#include <algorithm>
#include <cstring>
#include <mutex>

namespace dect2 { namespace hdr {

// ---------------------------------------------------------------- check sums

namespace {
struct CrcTables {
    uint8_t c8[256];
    uint16_t f16[256];
    CrcTables() {
        for (int i = 0; i < 256; i++) {
            unsigned c = (unsigned)i;
            for (int b = 0; b < 8; b++) c = (c & 0x80) ? ((c << 1) ^ 0x31) & 0xFF : (c << 1) & 0xFF;
            c8[i] = (uint8_t)c;
            unsigned f = (unsigned)i;
            for (int b = 0; b < 8; b++) f = (f & 1) ? (f >> 1) ^ 0x8408 : f >> 1;
            f16[i] = (uint16_t)f;
        }
    }
};
const CrcTables& crcTables() { static const CrcTables t; return t; }
}

uint8_t crc8(const uint8_t* p, size_t n) {
    const CrcTables& t = crcTables();
    unsigned crc = 0xFF;
    for (size_t i = 0; i < n; i++) crc = t.c8[crc ^ p[i]];
    return (uint8_t)crc;
}

uint16_t fcs16(const uint8_t* p, size_t n) {
    const CrcTables& t = crcTables();
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) crc = (uint16_t)((crc >> 8) ^ t.f16[(crc ^ p[i]) & 0xFF]);
    return crc;
}

void appendFcs(std::vector<uint8_t>& frame) {
    const uint16_t f = (uint16_t)~fcs16(frame.data(), frame.size());
    frame.push_back((uint8_t)(f & 0xFF));
    frame.push_back((uint8_t)(f >> 8));
}

// nrsc5 pids.c crc12(): the register runs over bits 67 down to 0, then 16 zero bits; the result is inverted with 0x955
uint16_t crc12(const uint8_t* bits) {
    const uint16_t poly = 0xD010;
    uint16_t reg = 0;
    for (int i = 67; i >= 0; i--) {
        const int low = reg & 1;
        reg >>= 1;
        reg ^= (uint16_t)((bits[i] & 1) << 15);
        if (low) reg ^= poly;
    }
    for (int i = 0; i < 16; i++) {
        const int low = reg & 1;
        reg >>= 1;
        if (low) reg ^= poly;
    }
    reg ^= 0x955;
    return reg & 0xFFF;
}

void putCrc12(uint8_t* bits80) {
    const uint16_t c = crc12(bits80);
    for (int i = 0; i < 12; i++) bits80[68 + i] = (uint8_t)((c >> (11 - i)) & 1);
}

// ---------------------------------------------------------------- scrambler (nrsc5 decode.c descramble())

void scramble(uint8_t* bits, size_t n) {
    unsigned val = 0x3FF;
    for (size_t i = 0; i < n; i++) {
        const unsigned bit = ((val >> 9) ^ val) & 1;
        val |= bit << 11;
        val >>= 1;
        bits[i] ^= (uint8_t)bit;
    }
}

// ---------------------------------------------------------------- convolutional codes

const ConvCode kCodeFm = {7, {0133, 0171, 0165}};
const ConvCode kCodeE1 = {9, {0561, 0657, 0711}};
const ConvCode kCodeE2 = {9, {0561, 0753, 0711}};
const uint8_t kPunct25[6] = {1, 1, 1, 1, 1, 0};
const uint8_t kPunctE1[15] = {1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1};
const uint8_t kPunctE2[6] = {1, 0, 1, 1, 0, 0};

namespace {
inline int parity(unsigned x) { return __builtin_parity(x); }
}

// The register holds the newest bit at position k-1 (nrsc5 decode.c bit_errors())
void convEncode(const ConvCode& c, const uint8_t* bits, size_t n, uint8_t* out) {
    const int k = c.k;
    unsigned r = 0;
    for (int i = 0; i < k - 1; i++) r = (r >> 1) | ((unsigned)(bits[(n - (size_t)(k - 1) + (size_t)i) % n] & 1) << (k - 1));
    for (size_t i = 0; i < n; i++) {
        r = (r >> 1) | ((unsigned)(bits[i] & 1) << (k - 1));
        out[3 * i] = (uint8_t)parity(r & c.gen[0]);
        out[3 * i + 1] = (uint8_t)parity(r & c.gen[1]);
        out[3 * i + 2] = (uint8_t)parity(r & c.gen[2]);
    }
}

int reencodeErrors(const ConvCode& c, const float* soft, const uint8_t* bits, size_t n, int* counted) {
    std::vector<uint8_t> enc(3 * n);
    convEncode(c, bits, n, enc.data());
    int err = 0, cnt = 0;
    for (size_t i = 0; i < 3 * n; i++) {
        if (soft[i] == 0.f) continue;
        cnt++;
        if ((soft[i] > 0) != (enc[i] != 0)) err++;
    }
    if (counted) *counted = cnt;
    return err;
}

// Tail-biting decoding by wrapping around: the trellis runs over the last W bits, the whole word and the first W bits again, from equal
// start metrics; the decisions of the middle part are the result.
void Viterbi::decode(const ConvCode& c, const float* soft, size_t n, uint8_t* out) {
    if (!n) return;
    const int K = c.k, S = 1 << (K - 1), half = S >> 1;
    const int W = K <= 7 ? 72 : 120;
    const size_t T = n + 2 * (size_t)W;
    const int words = std::max(1, S / 64);
    m0_.assign((size_t)S, 0.f);
    m1_.assign((size_t)S, 0.f);
    dec_.assign(T * (size_t)words, 0);
    // the three output bits of every register value
    uint8_t outTab[512];
    for (unsigned r = 0; r < (1u << K); r++) outTab[r] = (uint8_t)(parity(r & c.gen[0]) | (parity(r & c.gen[1]) << 1) | (parity(r & c.gen[2]) << 2));
    float* old = m0_.data();
    float* nw = m1_.data();
    for (size_t t = 0; t < T; t++) {
        const size_t bi = (t + n * 4 - (size_t)W) % n;
        const float s0 = soft[3 * bi], s1 = soft[3 * bi + 1], s2 = soft[3 * bi + 2];
        float bm[8];
        for (int o = 0; o < 8; o++) bm[o] = ((o & 1) ? s0 : -s0) + ((o & 2) ? s1 : -s1) + ((o & 4) ? s2 : -s2);
        uint64_t* d = &dec_[t * (size_t)words];
        // new state s' = (u << (K-2)) | (s >> 1); its predecessors are s = ((s' mod half) << 1) | b
        for (int sp = 0; sp < S; sp++) {
            const unsigned u = (unsigned)sp >> (K - 2);
            const unsigned base = ((unsigned)sp & (unsigned)(half - 1)) << 1;
            const unsigned ra = (u << (K - 1)) | base, rb = ra | 1u;
            const float a = old[base] + bm[outTab[ra]];
            const float b = old[base | 1] + bm[outTab[rb]];
            if (b > a) { nw[sp] = b; d[sp >> 6] |= (uint64_t)1 << (sp & 63); }
            else nw[sp] = a;
        }
        std::swap(old, nw);
        if ((t & 31) == 31) {
            const float m = old[0];
            for (int s = 0; s < S; s++) old[s] -= m;
        }
    }
    int best = 0;
    for (int s = 1; s < S; s++) if (old[s] > old[best]) best = s;
    unsigned st = (unsigned)best;
    for (size_t t = T; t-- > 0;) {
        const unsigned u = st >> (K - 2);
        if (t >= (size_t)W && t < (size_t)W + n) out[t - (size_t)W] = (uint8_t)u;
        const unsigned b = (unsigned)((dec_[t * (size_t)words + (st >> 6)] >> (st & 63)) & 1);
        st = ((st & (unsigned)(half - 1)) << 1) | b;
    }
}

// ---------------------------------------------------------------- Reed-Solomon (255, 247)

namespace {
struct Gf {
    uint8_t exp[512], log[256];
    uint8_t g[9];                    // generator, g[i] is the coefficient of x^i; roots alpha^1 .. alpha^8
    Gf() {
        unsigned x = 1;
        for (int i = 0; i < 255; i++) { exp[i] = (uint8_t)x; log[x] = (uint8_t)i; x <<= 1; if (x & 0x100) x ^= 0x11D; }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
        log[0] = 0;
        uint8_t p[9] = {1};
        int deg = 0;
        for (int r = 1; r <= 8; r++) {               // multiply by (x + alpha^r)
            uint8_t q[9] = {};
            for (int i = 0; i <= deg; i++) {
                q[i + 1] ^= p[i];
                q[i] ^= mul(p[i], exp[r]);
            }
            deg++;
            memcpy(p, q, sizeof p);
        }
        memcpy(g, p, sizeof g);
    }
    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? exp[log[a] + log[b]] : 0; }
    uint8_t div(uint8_t a, uint8_t b) const { return a ? exp[(log[a] + 255 - log[b]) % 255] : 0; }
    uint8_t pw(int e) const { e %= 255; if (e < 0) e += 255; return exp[e]; }
};
const Gf& gf() { static const Gf g; return g; }
}

// buf[i] is the coefficient of x^i of the code word (nrsc5 frame.c fix_header() maps it so into Karn's decoder)
void rsEncodeHeader(uint8_t* buf96) {
    const Gf& G = gf();
    uint8_t w[96];
    memcpy(w, buf96, 96);
    memset(w, 0, 8);
    for (int d = 95; d >= 8; d--) {
        const uint8_t coef = w[d];
        if (!coef) continue;
        for (int j = 0; j <= 8; j++) w[d - 8 + j] ^= G.mul(coef, G.g[j]);
    }
    memcpy(buf96, w, 8);
}

bool rsDecodeHeader(uint8_t* buf96, int* corrected) {
    const Gf& G = gf();
    if (corrected) *corrected = 0;
    uint8_t S[8];
    bool any = false;
    for (int j = 0; j < 8; j++) {
        // r(alpha^(j+1)) by Horner from the highest degree
        uint8_t v = 0;
        const uint8_t a = G.pw(j + 1);
        for (int i = 95; i >= 0; i--) v = (uint8_t)(G.mul(v, a) ^ buf96[i]);
        S[j] = v;
        if (v) any = true;
    }
    if (!any) return true;
    // Berlekamp-Massey
    uint8_t L[9] = {1}, B[9] = {1};
    int Ll = 0, m = 1;
    uint8_t b = 1;
    for (int nn = 0; nn < 8; nn++) {
        uint8_t dlt = S[nn];
        for (int i = 1; i <= Ll; i++) dlt ^= G.mul(L[i], S[nn - i]);
        if (!dlt) { m++; continue; }
        uint8_t Tm[9];
        memcpy(Tm, L, sizeof Tm);
        const uint8_t coef = G.div(dlt, b);
        for (int i = 0; i + m <= 8; i++) L[i + m] ^= G.mul(coef, B[i]);
        if (2 * Ll <= nn) { Ll = nn + 1 - Ll; memcpy(B, Tm, sizeof B); b = dlt; m = 1; }
        else m++;
    }
    if (Ll > 4) return false;
    // Omega = S * Lambda mod x^8
    uint8_t Om[8] = {};
    for (int i = 0; i < 8; i++) for (int j = 0; j <= i && j <= Ll; j++) Om[i] ^= G.mul(L[j], S[i - j]);
    // Chien search over the 96 real positions (an error in the shortened zeros means failure: the root count would not match)
    int found = 0;
    uint8_t fix[96] = {};
    for (int i = 0; i < 255; i++) {
        const uint8_t xinv = G.pw(-i);
        uint8_t v = 0;
        for (int j = Ll; j >= 0; j--) v = (uint8_t)(G.mul(v, xinv) ^ L[j]);
        if (v) continue;
        if (i >= 96) return false;
        // Forney with the first root alpha^1: e = Omega(X^-1) / Lambda'(X^-1)
        uint8_t num = 0, den = 0;
        for (int j = 7; j >= 0; j--) num = (uint8_t)(G.mul(num, xinv) ^ Om[j]);
        for (int j = 1; j <= Ll; j += 2) den ^= G.mul(L[j], G.pw(-i * (j - 1)));
        if (!den) return false;
        fix[i] = G.div(num, den);
        found++;
    }
    if (found != Ll) return false;
    for (int i = 0; i < 96; i++) buf96[i] ^= fix[i];
    if (corrected) *corrected = found;
    return true;
}

// ---------------------------------------------------------------- interleavers

namespace {
// nrsc5 decode.c PM_V: the order of the 20 partitions
const int kPmV[20] = {10, 2, 18, 6, 14, 8, 16, 0, 12, 4, 11, 3, 19, 7, 15, 9, 17, 1, 13, 5};
}

const int kBlDelay[3] = {2, 1, 5}, kMlDelay[3] = {11, 6, 7}, kBuDelay[3] = {10, 8, 9}, kMuDelay[3] = {4, 3, 0};
const int kElDelay[2] = {0, 1}, kEuDelay[4] = {2, 3, 5, 4};
const int kPidsIlDelay[12] = {0, 1, 12, 13, 6, 5, 18, 17, 11, 7, 23, 19};
const int kPidsIuDelay[12] = {2, 4, 14, 16, 3, 8, 15, 20, 9, 10, 21, 22};

// nrsc5 decode.c interleaver_i() with J = 20 partitions, B = 16 blocks, C = 36 columns, M = 1
const std::vector<int>& fmP1Pos() {
    static const std::vector<int> v = [] {
        const int J = 20, B = 16, C = 36;
        std::vector<int> p((size_t)kP1EncFm);
        for (int i = 0; i < kP1EncFm; i++) {
            const int part = kPmV[i % 20];
            const int block = ((i / J) + part * 7) % B;
            const int k = i / (J * B);
            const int row = (k * 11) % 32;
            const int col = (k * 11 + k / (32 * 9)) % C;
            p[(size_t)i] = (block * 32 + row) * (J * C) + part * C + col;
        }
        return p;
    }();
    return v;
}

// nrsc5 decode.c interleaver_ii(): the 200 PIDS bits of one block, after the P1 bits (I0 = kP1EncFm)
const std::vector<int>& fmPidsPos() {
    static const std::vector<int> v = [] {
        const int J = 20, B = 16, C = 36, b = kPidsEncFm;
        std::vector<int> p((size_t)b);
        for (int i = 0; i < b; i++) {
            const int part = kPmV[i % 20];
            const int k = ((i / J) % (b / J)) + (kP1EncFm / (J * B));
            const int row = (k * 11) % 32;
            const int col = (k * 11 + k / (32 * 9)) % C;
            p[(size_t)i] = row * (J * C) + part * C + col;
        }
        return p;
    }();
    return v;
}

// nrsc5 decode.c interleaver_iv(): where the read of epoch position g comes from in the interleaver memory (N = 32 blocks of bits)
namespace {
const std::vector<int>& pxPos(int frameLen) {
    static std::mutex mu;
    static std::vector<int> v2, v3;
    std::lock_guard<std::mutex> lk(mu);
    std::vector<int>& v = frameLen == kP3LenMp3 ? v3 : v2;
    if (!v.empty()) return v;
    const int J = frameLen == kP3LenMp3 ? 4 : 2, B = 32, C = 36, M = frameLen == kP3LenMp3 ? 2 : 4;
    const int N = frameLen == kP3LenMp3 ? 147456 : 73728;
    const int bkBits = 32 * C, bkAdj = 32 * C - 1;
    int pt[4] = {0, 0, 0, 0};
    v.resize((size_t)N);
    for (int g = 0; g < N; g++) {
        const int part = ((g + 2 * (M / 4)) / M) % J;
        const int pti = pt[part]++;
        const int block = (pti + part * 7 - bkAdj * (pti / bkBits)) % B;
        const int row = ((11 * pti) % bkBits) / C;
        const int col = (pti * 11) % C;
        v[(size_t)g] = (block * 32 + row) * (J * C) + part * C + col;
    }
    return v;
}
int pxN(int frameLen) { return frameLen == kP3LenMp3 ? 147456 : 73728; }
}

const uint8_t kPunctP3[6] = {1, 0, 1, 1, 0, 1};

void PxDeinterleaver::reset(int frameLen) {
    len_ = frameLen;
    n_ = pxN(frameLen);
    i_ = 0;
    ready_ = false;
    mem_.assign((size_t)n_, 0.f);
    pxPos(frameLen);
}

bool PxDeinterleaver::push(const float* in, std::vector<float>& out) {
    const std::vector<int>& pos = pxPos(len_);
    if (i_ == n_) { i_ = 0; ready_ = true; }
    out.assign((size_t)len_ * 3, 0.f);
    size_t o = 0;
    for (int x = 0; x < 2 * len_; x++) {
        out[o++] = mem_[(size_t)pos[(size_t)i_]];
        if (o % 6 == 1 || o % 6 == 4) out[o++] = 0.f;     // depuncture 1 0 1 1 0 1
        mem_[(size_t)i_] = in[x];
        i_++;
    }
    return ready_;
}

void PxInterleaver::reset(int frameLen) {
    len_ = frameLen;
    n_ = pxN(frameLen);
    g_ = t_ = 0;
    ring_.assign((size_t)(2 * n_ + 8 * frameLen), 0);
    pxPos(frameLen);
}

bool PxInterleaver::wantsFrame() const { return g_ < t_ + 2 * len_ + n_; }

// The read of code word bit G (epoch position g = G mod N) takes memory cell k = pos(g), written k bits into the same epoch when k < g,
// else in the epoch before: that is when it has to be sent.
void PxInterleaver::addFrame(const uint8_t* kept) {
    const std::vector<int>& pos = pxPos(len_);
    const int64_t R = (int64_t)ring_.size();
    for (int x = 0; x < 2 * len_; x++, g_++) {
        const int64_t e = g_ / n_, g = g_ % n_, k = pos[(size_t)g];
        const int64_t w = (k < g ? e : e - 1) * n_ + k;
        if (w >= t_) ring_[(size_t)(w % R)] = kept[x];
    }
}

void PxInterleaver::call(uint8_t* out) {
    const int64_t R = (int64_t)ring_.size();
    for (int x = 0; x < 2 * len_; x++, t_++) {
        uint8_t& c = ring_[(size_t)(t_ % R)];
        out[x] = c;
        c = 0;
    }
}

// nrsc5 decode.c interleaver_ma1() and decode_process_pids_am() (1012s section 10.4)
const AmMaps& amMaps() {
    static const AmMaps m = [] {
        AmMaps a;
        auto at = [](int b, int k, int p) {
            const int col = (9 * k) % 25;
            const int row = (11 * col + 16 * (k / 25) + 11 * (k / 50)) % 32;
            return AmBit{(b * 32 + row) * kAmCols + col, p};
        };
        for (int n = 0; n < 18000; n++) {
            a.bl.push_back(at(n / 2250, (n + n / 750 + 1) % 750, n % 3));
            a.ml.push_back(at((3 * n + 3) % 8, (n + n / 3000 + 3) % 750, 3 + n % 3));
            a.bu.push_back(at(n / 2250, (n + n / 750) % 750, n % 3));
            a.mu.push_back(at((3 * n) % 8, (n + n / 3000 + 2) % 750, 3 + n % 3));
        }
        for (int n = 0; n < 12000; n++) a.el.push_back(at((3 * n + n / 3000) % 8, (n + n / 6000) % 750, n % 2));
        for (int n = 0; n < 24000; n++) a.eu.push_back(at((3 * n + n / 3000 + 2 * (n / 12000)) % 8, (n + n / 6000) % 750, n % 4));
        for (int n = 0; n < 120; n++) {
            const int p = n % 4;
            int k = (n + n / 60 + 11) % 30;
            a.pidsRowL.push_back((11 * (k + k / 15) + 3) % 32); a.pidsBitL.push_back(p);
            k = (n + n / 60) % 30;
            a.pidsRowU.push_back((11 * (k + k / 15) + 3) % 32); a.pidsBitU.push_back(p);
        }
        for (int k = 750; k < 800; k++) {
            const int col = (9 * k) % 25;
            const int row = (11 * col + 16 * (k / 25) + 11 * (k / 50)) % 32;
            a.trainRow[col].push_back(row);
        }
        return a;
    }();
    return m;
}

}} // namespace dect2::hdr
