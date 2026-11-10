// More CubeSat framings (U482C, CCSDS concatenated, USP, NGHam, Mobitex): see cubesat_more.h.
// Re-implemented after gr-satellites (Daniel Estevez EA4GPZ and contributors, GPL-3.0-or-later): u482c_decode_impl.cc, viterbi.c,
// ccsds_concatenated_deframer.py, ccsds_rs_deframer.py, usp_deframer.py, usp/usp_pls_crop.py, usp/usp_ax25_crop.py, ngham_deframer.py,
// ngham_packet_crop.py, ngham_remove_padding.py, mobitex_deframer.py, mobitex_to_datablocks.py, mobitex_fec.py, mobitex_fec_block.py,
// tubix20_reframer.py; NGHam after ngham.c of the NGHam reference code (Jon Petter Skagmo LA3JPA, LGPL-3.0).
#include "dect2/cubesat/cubesat_more.h"
#include "dect2/cubesat/cubesat_proto.h"
#include <algorithm>
#include <cstring>
#include <deque>

namespace dect2 {
namespace cubesat {

namespace {

inline int par(uint32_t v) { v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return (int)(v & 1u); }
inline int popc64(uint64_t v) { return __builtin_popcountll(v); }

// the two outputs of the code for a 7 bit register (newest bit lowest): bit 0 the first symbol, bit 1 the second
inline int convOut(int kind, unsigned reg) {
    const int p79 = par(reg & 79u), p109 = par(reg & 109u);
    switch (kind) {
    case ConvCcsds: return p79 | ((p109 ^ 1) << 1);
    case ConvNasaDsn: return (p109 ^ 1) | (p79 << 1);
    case ConvCcsdsUninv: return p79 | (p109 << 1);
    default: return p109 | (p79 << 1);           // NASA-DSN uninverted (U482C: 0x6D, 0x4F)
    }
}

std::vector<uint8_t> bitsOf(const uint8_t* d, size_t n) {
    std::vector<uint8_t> b;
    b.reserve(n * 8);
    for (size_t i = 0; i < n; i++) for (int k = 7; k >= 0; k--) b.push_back((uint8_t)((d[i] >> k) & 1));
    return b;
}
std::vector<uint8_t> packBits(const std::vector<uint8_t>& b, size_t nbits) {
    std::vector<uint8_t> v((nbits + 7) / 8, 0);
    for (size_t i = 0; i < nbits && i < b.size(); i++) if (b[i]) v[i >> 3] = (uint8_t)(v[i >> 3] | (0x80 >> (i & 7)));
    return v;
}
void putU32(std::vector<uint8_t>& v, uint32_t a) { for (int k = 3; k >= 0; k--) v.push_back((uint8_t)(a >> (8 * k))); }

// ---- GF(2^8), 0x187
struct Gf {
    uint8_t exp[512], log[256];
    Gf() {
        int x = 1;
        for (int i = 0; i < 255; i++) { exp[i] = (uint8_t)x; log[x] = (uint8_t)i; x <<= 1; if (x & 0x100) x ^= 0x187; }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
        log[0] = 0;
    }
    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? exp[log[a] + log[b]] : 0; }
    uint8_t div(uint8_t a, uint8_t b) const { return a ? exp[(log[a] + 255 - log[b]) % 255] : 0; }
    uint8_t pw(int e) const { e %= 255; if (e < 0) e += 255; return exp[e]; }
};
const Gf& gf() { static const Gf g; return g; }

std::vector<uint8_t> genPoly(int nroots) {
    const Gf& G = gf();
    std::vector<uint8_t> g(1, 1);
    for (int i = 0; i < nroots; i++) {
        const uint8_t r = G.pw(11 * (112 + i));
        std::vector<uint8_t> n(g.size() + 1, 0);
        for (size_t j = 0; j < g.size(); j++) { n[j] ^= g[j]; n[j + 1] ^= G.mul(g[j], r); }
        g = n;
    }
    return g;
}

// ---- CRC
uint16_t crcRefl(const uint8_t* d, size_t n, uint16_t init) {
    uint16_t c = init;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1);
    }
    return c;
}

} // namespace

uint16_t crc16X25(const uint8_t* d, size_t n) { return (uint16_t)(crcRefl(d, n, 0xFFFF) ^ 0xFFFF); }
uint16_t crc16Xmodem(const uint8_t* d, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)(d[i] << 8);
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

// ================================================================ convolutional code
std::vector<uint8_t> convEncode(const std::vector<uint8_t>& bits, int kind, uint32_t* state) {
    uint32_t s = state ? *state : 0;
    std::vector<uint8_t> out;
    out.reserve(bits.size() * 2);
    for (uint8_t b : bits) {
        const unsigned reg = ((s << 1) | (b & 1u)) & 0x7Fu;
        const int o = convOut(kind, reg);
        out.push_back((uint8_t)(o & 1)); out.push_back((uint8_t)(o >> 1));
        s = reg & 0x3Fu;
    }
    if (state) *state = s;
    return out;
}

bool convViterbi(const float* soft, size_t n, int kind, bool terminated, std::vector<uint8_t>& bits) {
    const size_t nb = n / 2;
    if (nb < 1) return false;
    uint8_t out[64][2];
    for (int s = 0; s < 64; s++) for (int b = 0; b < 2; b++) out[s][b] = (uint8_t)convOut(kind, (((unsigned)s << 1) | (unsigned)b) & 0x7Fu);
    std::vector<float> pm(64, 1e30f), npm(64);
    pm[0] = 0;
    std::vector<uint8_t> prev(nb * 64);
    for (size_t t = 0; t < nb; t++) {
        const float s0 = soft[2 * t], s1 = soft[2 * t + 1];
        const float c[4] = {s0 + s1, -s0 + s1, s0 - s1, -s0 - s1};
        std::fill(npm.begin(), npm.end(), 1e30f);
        for (int s = 0; s < 64; s++) {
            if (pm[(size_t)s] >= 1e29f) continue;
            for (int b = 0; b < 2; b++) {
                const int ns = ((s << 1) | b) & 0x3F;
                const float v = pm[(size_t)s] + c[out[s][b]];
                if (v < npm[(size_t)ns]) { npm[(size_t)ns] = v; prev[t * 64 + (size_t)ns] = (uint8_t)s; }
            }
        }
        pm.swap(npm);
    }
    int st = 0;
    if (!terminated) { float best = 1e30f; for (int s = 0; s < 64; s++) if (pm[(size_t)s] < best) { best = pm[(size_t)s]; st = s; } }
    if (pm[(size_t)st] >= 1e29f) return false;
    bits.assign(nb, 0);
    for (size_t t = nb; t-- > 0;) { bits[t] = (uint8_t)(st & 1); st = prev[t * 64 + (size_t)st]; }
    return true;
}

ConvStream::ConvStream(int kind) { setKind(kind); }
void ConvStream::setKind(int kind) {
    kind_ = kind;
    for (int s = 0; s < 64; s++) for (int b = 0; b < 2; b++) outTab_[s][b] = (uint8_t)convOut(kind, (((unsigned)s << 1) | (unsigned)b) & 0x7Fu);
    reset();
}
void ConvStream::reset() {
    for (float& v : pm_) v = 0;
    dec_.clear();
    steps_ = 0;
    out_.clear();
}
void ConvStream::push(float s0, float s1) {
    const float c[4] = {s0 + s1, -s0 + s1, s0 - s1, -s0 - s1};
    float npm[64];
    uint64_t d = 0;
    float mn = 1e30f;
    for (int ns = 0; ns < 64; ns++) {
        const int b = ns & 1;
        const int p0 = ns >> 1, p1 = p0 | 32;
        const float v0 = pm_[p0] + c[outTab_[p0][b]], v1 = pm_[p1] + c[outTab_[p1][b]];
        if (v1 < v0) { npm[ns] = v1; d |= 1ull << ns; } else npm[ns] = v0;
        mn = std::min(mn, npm[ns]);
    }
    for (int s = 0; s < 64; s++) pm_[s] = npm[s] - mn;
    dec_.push_back(d);
    steps_++;
    constexpr size_t kTb = 96, kChunk = 64;
    if (dec_.size() >= kTb + kChunk) traceback(kChunk);
}
void ConvStream::traceback(size_t emit) {
    int st = 0;
    float best = 1e30f;
    for (int s = 0; s < 64; s++) if (pm_[s] < best) { best = pm_[s]; st = s; }
    std::vector<uint8_t> b(dec_.size());
    for (size_t t = dec_.size(); t-- > 0;) { b[t] = (uint8_t)(st & 1); st = (st >> 1) | (int)(((dec_[t] >> st) & 1u) << 5); }
    out_.insert(out_.end(), b.begin(), b.begin() + (ptrdiff_t)emit);
    dec_.erase(dec_.begin(), dec_.begin() + (ptrdiff_t)emit);
}

// ================================================================ Reed-Solomon, nroots parity bytes
void rsParityN(const uint8_t* data, size_t n, int nroots, uint8_t* par) {
    const Gf& G = gf();
    const std::vector<uint8_t> g = genPoly(nroots);
    std::vector<uint8_t> r((size_t)nroots, 0);
    for (size_t i = 0; i < n; i++) {
        const uint8_t fb = data[i] ^ r[0];
        std::memmove(r.data(), r.data() + 1, (size_t)nroots - 1);
        r[(size_t)nroots - 1] = 0;
        if (fb) for (int j = 0; j < nroots; j++) r[(size_t)j] ^= G.mul(fb, g[(size_t)j + 1]);
    }
    std::memcpy(par, r.data(), (size_t)nroots);
}

int rsDecodeN(uint8_t* c, size_t n, int nr) {
    if (nr < 2 || nr > 64 || n <= (size_t)nr || n > 255) return -1;
    const Gf& G = gf();
    std::vector<uint8_t> S((size_t)nr);
    bool any = false;
    for (int i = 0; i < nr; i++) {
        const uint8_t r = G.pw(11 * (112 + i));
        uint8_t s = 0;
        for (size_t j = 0; j < n; j++) s = G.mul(s, r) ^ c[j];
        S[(size_t)i] = s;
        any |= s != 0;
    }
    if (!any) return 0;
    std::vector<uint8_t> C((size_t)nr + 1, 0), B((size_t)nr + 1, 0), T;
    C[0] = B[0] = 1;
    int L = 0, m = 1;
    uint8_t b = 1;
    for (int k = 0; k < nr; k++) {
        uint8_t d = S[(size_t)k];
        for (int i = 1; i <= L; i++) d ^= G.mul(C[(size_t)i], S[(size_t)(k - i)]);
        if (d == 0) { m++; continue; }
        T = C;
        const uint8_t coef = G.div(d, b);
        for (int i = 0; i + m <= nr; i++) C[(size_t)(i + m)] ^= G.mul(coef, B[(size_t)i]);
        if (2 * L <= k) { L = k + 1 - L; B = T; b = d; m = 1; } else m++;
    }
    if (L > nr / 2) return -1;
    std::vector<uint8_t> Om((size_t)nr, 0);
    for (int i = 0; i < nr; i++) for (int j = 0; j <= i && j <= L; j++) Om[(size_t)i] ^= G.mul(C[(size_t)j], S[(size_t)(i - j)]);
    int found = 0;
    for (size_t j = 0; j < n; j++) {
        const int e = (int)(n - 1 - j);
        const int xi = (255 - (11 * e) % 255) % 255;
        uint8_t v = 0;
        for (int i = L; i >= 0; i--) v = G.mul(v, G.exp[xi]) ^ C[(size_t)i];
        if (v) continue;
        uint8_t om = 0;
        for (int i = nr - 1; i >= 0; i--) om = G.mul(om, G.exp[xi]) ^ Om[(size_t)i];
        uint8_t dl = 0;
        for (int i = 1; i <= L; i += 2) dl ^= G.mul(C[(size_t)i], G.pw(xi * (i - 1)));
        if (!dl) return -1;
        c[j] ^= G.mul(G.pw(11 * e * (1 - 112)), G.div(om, dl));
        found++;
    }
    if (found != L) return -1;
    for (int i = 0; i < nr; i++) {
        const uint8_t r = G.pw(11 * (112 + i));
        uint8_t s = 0;
        for (size_t j = 0; j < n; j++) s = G.mul(s, r) ^ c[j];
        if (s) return -1;
    }
    return found;
}

// ================================================================ U482C
std::vector<uint8_t> u482cEncode(const std::vector<uint8_t>& packet, int flags) {
    const bool rs = flags & kU482cRs, conv = flags & kU482cConv, scr = flags & kU482cScrambler;
    const size_t maxPay = conv ? (rs ? 94u : 126u) : (rs ? 223u : 255u);
    std::vector<uint8_t> p(packet.begin(), packet.begin() + (ptrdiff_t)std::min(packet.size(), maxPay));
    if (rs) { uint8_t par32[32]; rsParity(p.data(), p.size(), par32); p.insert(p.end(), par32, par32 + 32); }
    if (scr) ccsdsRandomize(p.data(), p.size());
    size_t frameLen = p.size();
    std::vector<uint8_t> body = p;
    if (conv) {
        std::vector<uint8_t> bits = bitsOf(p.data(), p.size());
        bits.insert(bits.end(), 6, 0);
        const std::vector<uint8_t> sym = convEncode(bits, ConvU482c);
        frameLen = 2 * (p.size() + 1);
        body = packBits(sym, frameLen * 8);
    }
    std::vector<uint8_t> v;
    putU32(v, kSyncU482c);
    const uint32_t g = golayEncode((uint32_t)(frameLen & 0xFF) | (uint32_t)(flags & 0x700));
    v.push_back((uint8_t)(g >> 16)); v.push_back((uint8_t)(g >> 8)); v.push_back((uint8_t)g);
    v.insert(v.end(), body.begin(), body.end());
    return v;
}

bool u482cDecode(const uint8_t* a, size_t n, std::vector<uint8_t>& packet, int& rsErr, int& flags) {
    rsErr = -1;
    if (n < 4) return false;
    const int lf = golayDecode(((uint32_t)a[0] << 16) | ((uint32_t)a[1] << 8) | a[2]);
    if (lf < 0) return false;
    flags = lf & 0x700;
    const size_t frameLen = (size_t)(lf & 0xFF);
    const uint8_t* body = a + 3;
    const size_t avail = n - 3;
    if (frameLen == 0 || frameLen > avail) return false;
    std::vector<uint8_t> p;
    if (flags & kU482cConv) {
        const int len = (int)frameLen / 2 - 1;
        if (len <= 0) return false;
        const size_t nsym = ((size_t)len * 8 + 6) * 2;
        std::vector<uint8_t> hb = bitsOf(body, frameLen);
        if (hb.size() < nsym) return false;
        std::vector<float> soft(nsym);
        for (size_t i = 0; i < nsym; i++) soft[i] = hb[i] ? 1.f : -1.f;
        std::vector<uint8_t> bits;
        if (!convViterbi(soft.data(), nsym, ConvU482c, true, bits)) return false;
        p = packBits(bits, (size_t)len * 8);
    } else {
        p.assign(body, body + frameLen);
    }
    if (flags & kU482cScrambler) ccsdsRandomize(p.data(), p.size());
    if (flags & kU482cRs) {
        if (p.size() <= 32) return false;
        rsErr = rsDecode(p.data(), p.size());
        if (rsErr < 0) return false;
        p.resize(p.size() - 32);
    }
    packet = std::move(p);
    return !packet.empty();
}

// ================================================================ CCSDS concatenated
static std::vector<uint8_t> concatBlock(const std::vector<uint8_t>& frame, const ConcatParams& p) {
    const int I = std::max(1, p.interleave);
    std::vector<uint8_t> d(frame.begin(), frame.begin() + (ptrdiff_t)std::min(frame.size(), (size_t)p.frameSize));
    d.resize((size_t)p.frameSize, 0);
    const size_t k = (size_t)p.frameSize / (size_t)I;
    std::vector<uint8_t> blk((size_t)p.blockBytes(), 0);
    for (int j = 0; j < I; j++) {
        std::vector<uint8_t> cw(k);
        for (size_t i = 0; i < k; i++) cw[i] = d[i * (size_t)I + (size_t)j];
        uint8_t par32[32];
        if (p.dualBasis) rsParityDual(cw.data(), k, par32); else rsParity(cw.data(), k, par32);
        for (size_t i = 0; i < k; i++) blk[i * (size_t)I + (size_t)j] = cw[i];
        for (size_t i = 0; i < 32; i++) blk[(k + i) * (size_t)I + (size_t)j] = par32[i];
    }
    if (p.scrambler) ccsdsRandomize(blk.data(), blk.size());
    return blk;
}

std::vector<uint8_t> ccsdsConcatEncode(const std::vector<uint8_t>& frame, const ConcatParams& p) {
    std::vector<uint8_t> by;
    putU32(by, kAsmCcsds);
    const std::vector<uint8_t> blk = concatBlock(frame, p);
    by.insert(by.end(), blk.begin(), blk.end());
    by.insert(by.end(), 4, 0x55);           // a little tail so the decoder's traceback reaches the end of the block
    std::vector<uint8_t> bits = bitsOf(by.data(), by.size());
    if (p.differential) { int prev = 0; for (uint8_t& b : bits) { prev ^= b; b = (uint8_t)prev; } }
    return convEncode(bits, p.conv);
}

bool ccsdsConcatDecode(const uint8_t* block, const ConcatParams& p, std::vector<uint8_t>& frame, int& rsErr) {
    const int I = std::max(1, p.interleave);
    if (p.frameSize % I || p.frameSize / I > 223 || p.frameSize < I) return false;
    std::vector<uint8_t> b(block, block + p.blockBytes());
    if (p.scrambler) ccsdsRandomize(b.data(), b.size());
    const size_t nn = b.size() / (size_t)I;
    rsErr = 0;
    frame.assign((size_t)p.frameSize, 0);
    for (int j = 0; j < I; j++) {
        std::vector<uint8_t> cw(nn);
        for (size_t i = 0; i < nn; i++) cw[i] = b[i * (size_t)I + (size_t)j];
        const int e = p.dualBasis ? rsDecodeDual(cw.data(), nn) : rsDecode(cw.data(), nn);
        if (e < 0) return false;
        rsErr += e;
        for (size_t i = 0; i < nn - 32; i++) frame[i * (size_t)I + (size_t)j] = cw[i];
    }
    return true;
}

// ================================================================ USP
const uint8_t kUspSync[8] = {80, 114, 246, 75, 45, 144, 177, 245};
namespace {
constexpr uint64_t kUspPls[2] = {0x719D83C953422DFAull, 0x24C8D69C061778AFull};   // 48 and 223 data bytes (scrambled PLS codes)
}

std::vector<uint8_t> uspEncode(const std::vector<uint8_t>& ax25) {
    const size_t len = std::min(ax25.size(), (size_t)219);
    const int code = len + 4 <= 48 ? 0 : 1;
    const size_t dataLen = code ? 223 : 48;
    std::vector<uint8_t> d(dataLen, 0);
    d[0] = 0; d[1] = 0;
    d[2] = (uint8_t)(len & 0xFF); d[3] = (uint8_t)(len >> 8);
    std::copy(ax25.begin(), ax25.begin() + (ptrdiff_t)len, d.begin() + 4);
    uint8_t par32[32];
    rsParityDual(d.data(), d.size(), par32);
    d.insert(d.end(), par32, par32 + 32);
    ccsdsRandomize(d.data(), d.size());
    std::vector<uint8_t> out = bitsOf(kUspSync, 8);
    for (int k = 63; k >= 0; k--) out.push_back((uint8_t)((kUspPls[code] >> k) & 1u));
    const std::vector<uint8_t> sym = convEncode(bitsOf(d.data(), d.size()), ConvCcsds);
    out.insert(out.end(), sym.begin(), sym.end());
    return out;
}

bool uspDecode(const float* soft, size_t n, std::vector<uint8_t>& ax25, int& rsErr, int* dataLenOut) {
    rsErr = -1;
    if (n < (size_t)kUspPlsBits + 2 * 8 * 80) return false;
    float corr[2] = {0, 0};
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < 64; k++) corr[c] += soft[k] * (((kUspPls[c] >> (63 - k)) & 1u) ? 1.f : -1.f);
    const int code = corr[1] > corr[0] ? 1 : 0;
    const size_t dataLen = code ? 223 : 48;
    const size_t nsym = (dataLen + 32) * 16;
    if (n < 64 + nsym) return false;
    std::vector<uint8_t> bits;
    if (!convViterbi(soft + 64, nsym, ConvCcsds, false, bits)) return false;
    std::vector<uint8_t> d = packBits(bits, (dataLen + 32) * 8);
    ccsdsRandomize(d.data(), d.size());
    rsErr = rsDecodeDual(d.data(), d.size());
    if (rsErr < 0) return false;
    const size_t len = (size_t)d[2] | ((size_t)d[3] << 8);
    if (len == 0 || len + 4 > dataLen) return false;
    ax25.assign(d.begin() + 4, d.begin() + 4 + (ptrdiff_t)len);
    if (dataLenOut) *dataLenOut = (int)dataLen;
    return true;
}

// ================================================================ NGHam
namespace {
const uint32_t kNghTag[7] = {0x3B49CD, 0x4DDA57, 0x76939A, 0x9BB4AE, 0xA0FD63, 0xD66EF9, 0xED2734};
const int kNghPl[7] = {28, 60, 92, 124, 156, 188, 220};
const int kNghFull[7] = {31, 63, 95, 127, 159, 191, 223};
const int kNghCw[7] = {47, 79, 111, 159, 191, 223, 255};
}
int nghamCodewordBytes(int i) { return i >= 0 && i < 7 ? kNghCw[i] : 0; }

std::vector<uint8_t> nghamEncode(const std::vector<uint8_t>& payload, int flags, bool rs) {
    const size_t len = std::min(payload.size(), (size_t)220);
    int sz = 0;
    while ((int)len > kNghPl[sz]) sz++;
    std::vector<uint8_t> v;
    putU32(v, kSyncNgham);
    v.push_back((uint8_t)(kNghTag[sz] >> 16)); v.push_back((uint8_t)(kNghTag[sz] >> 8)); v.push_back((uint8_t)kNghTag[sz]);
    std::vector<uint8_t> cw;
    cw.push_back((uint8_t)(((kNghPl[sz] - (int)len) & 0x1F) | ((flags << 5) & 0xE0)));
    cw.insert(cw.end(), payload.begin(), payload.begin() + (ptrdiff_t)len);
    const uint16_t crc = crc16X25(cw.data(), cw.size());
    cw.push_back((uint8_t)(crc >> 8)); cw.push_back((uint8_t)crc);
    cw.resize((size_t)kNghFull[sz], 0);
    const int nr = kNghCw[sz] - kNghFull[sz];
    std::vector<uint8_t> parity((size_t)nr, 0);
    if (rs) rsParityN(cw.data(), cw.size(), nr, parity.data());
    cw.insert(cw.end(), parity.begin(), parity.end());       // the "no RS" variant still sends the parity space (zeros)
    ccsdsRandomize(cw.data(), cw.size());
    v.insert(v.end(), cw.begin(), cw.end());
    return v;
}

bool nghamDecode(const uint8_t* a, size_t n, std::vector<uint8_t>& payload, int& rsErr, int* flagsOut) {
    rsErr = -1;
    if (n < 3 + 47) return false;
    const uint32_t tag = ((uint32_t)a[0] << 16) | ((uint32_t)a[1] << 8) | a[2];
    int best = -1, bd = 99;
    for (int i = 0; i < 7; i++) { const int d = __builtin_popcount(tag ^ kNghTag[i]); if (d < bd) { bd = d; best = i; } }
    if (bd > 6) return false;
    const size_t cwLen = (size_t)kNghCw[best];
    if (n < 3 + cwLen) return false;
    std::vector<uint8_t> cw(a + 3, a + 3 + cwLen);
    ccsdsRandomize(cw.data(), cw.size());
    auto check = [&](const std::vector<uint8_t>& c) -> bool {
        const int pad = c[0] & 0x1F;
        const int len = kNghPl[best] - pad;
        if (len < 0) return false;
        const uint16_t crc = crc16X25(c.data(), (size_t)len + 1);
        if (crc != (((uint16_t)c[(size_t)len + 1] << 8) | c[(size_t)len + 2])) return false;
        payload.assign(c.begin() + 1, c.begin() + 1 + len);
        if (flagsOut) *flagsOut = c[0] >> 5;
        return true;
    };
    if (check(cw)) { rsErr = 0; return true; }                        // clean (or sent without RS)
    std::vector<uint8_t> c2 = cw;
    const int e = rsDecodeN(c2.data(), c2.size(), (int)cwLen - kNghFull[best]);
    if (e < 0 || !check(c2)) return false;
    rsErr = e;
    return true;
}

// ================================================================ Mobitex
namespace {
const uint8_t kMobFec8[4] = {0xEC, 0xD3, 0xBA, 0x75};
const uint16_t kMobFec12[4] = {0xEC8, 0xD34, 0xBA2, 0x751};

int mobEncode12(uint8_t m) {
    int fec = 0;
    for (int i = 0; i < 4; i++) fec |= par(m & kMobFec8[i]) << (3 - i);
    return (m << 4) | fec;
}
// 0 no error, 1 corrected, 2 uncorrectable
int mobDecode12(int cw, uint8_t& m) {
    int syn = 0;
    for (int i = 0; i < 4; i++) syn |= par((uint32_t)(cw & kMobFec12[i])) << (3 - i);
    if (syn == 0) { m = (uint8_t)(cw >> 4); return 0; }
    for (int pos = 0; pos < 12; pos++) {
        int s = 0;
        for (int i = 0; i < 4; i++) s |= par((uint32_t)((1 << pos) & kMobFec12[i])) << (3 - i);
        if (s == syn) { m = (uint8_t)((cw ^ (1 << pos)) >> 4); return 1; }
    }
    m = (uint8_t)(cw >> 4);
    return 2;
}

// the additive scrambler (GNU Radio lfsr: mask 0x22, seed 0x1FF, length 9)
struct MobLfsr {
    uint32_t sr = 0x1FF;
    int next() {
        const int out = (int)(sr & 1u);
        const uint32_t nb = (uint32_t)par(sr & 0x22u);
        sr = (sr >> 1) | (nb << 9);
        return out;
    }
};

int mobIdx(int i) { return (i % 12) * 20 + i / 12; }   // np.arange(240).reshape(12, 20).T.ravel()

constexpr size_t kMobBlock = 30;
size_t mobHeader(int variant) { return variant == MobitexBeesat1 ? 3 : 11; }
} // namespace

size_t mobitexMaxBytes(int variant) { return mobHeader(variant) + 32 * kMobBlock; }

std::vector<uint8_t> mobitexEncode(const MobitexFrame& f, bool nx, int variant) {
    const size_t nb = std::max((size_t)1, std::min((size_t)32, f.blocks.size()));
    std::vector<uint8_t> v;
    const uint16_t sync = nx ? kSyncMobitexNx : kSyncMobitex;
    v.push_back((uint8_t)(sync >> 8)); v.push_back((uint8_t)sync);
    const uint8_t c0 = (uint8_t)((f.control[0] & 0xE0) | ((nb - 1) & 0x1F)), c1 = f.control[1];
    v.push_back(c0); v.push_back(c1);
    v.push_back((uint8_t)(((mobEncode12(c0) & 0xF) << 4) | (mobEncode12(c1) & 0xF)));
    if (variant != MobitexBeesat1) {
        std::string cs = f.callsign;
        cs.resize(6, ' ');
        const uint16_t crc = crc16Xmodem((const uint8_t*)cs.data(), 6);
        v.insert(v.end(), cs.begin(), cs.end());
        v.push_back((uint8_t)(crc >> 8)); v.push_back((uint8_t)crc);
    }
    MobLfsr lf;
    for (size_t b = 0; b < nb; b++) {
        std::vector<uint8_t> d = b < f.blocks.size() ? f.blocks[b] : std::vector<uint8_t>();
        d.resize(18, 0);
        const uint16_t crc = crc16X25(d.data(), 18);
        d.push_back((uint8_t)(crc >> 8)); d.push_back((uint8_t)crc);
        std::vector<uint8_t> coded;
        for (size_t i = 0; i < 20; i += 2) {
            const int w0 = mobEncode12(d[i]), w1 = mobEncode12(d[i + 1]);
            coded.push_back((uint8_t)(w0 >> 4)); coded.push_back((uint8_t)(((w0 & 0xF) << 4) | (w1 >> 8))); coded.push_back((uint8_t)w1);
        }
        const std::vector<uint8_t> y = bitsOf(coded.data(), coded.size());     // the interleaver's output
        std::vector<uint8_t> x(240);
        for (int i = 0; i < 240; i++) x[(size_t)mobIdx(i)] = y[(size_t)i];
        for (uint8_t& bit : x) bit = (uint8_t)(bit ^ lf.next());
        const std::vector<uint8_t> by = packBits(x, 240);
        v.insert(v.end(), by.begin(), by.end());
    }
    return v;
}

bool mobitexDecode(const uint8_t* a, size_t n, int variant, MobitexFrame& f, int callsignBits) {
    const size_t hl = mobHeader(variant);
    if (n < hl + kMobBlock) return false;
    f = MobitexFrame();
    uint8_t c0, c1;
    const int s0 = mobDecode12((a[0] << 4) | (a[2] >> 4), c0), s1 = mobDecode12((a[1] << 4) | (a[2] & 0xF), c1);
    if (s0 == 2 || s1 == 2) return false;
    f.bitErrors = (s0 == 1) + (s1 == 1);
    f.control[0] = c0; f.control[1] = c1;
    const size_t nb = variant == MobitexBeesat9 ? 32 : (size_t)(c0 & 0x1F) + 1;
    uint8_t csBytes[8] = {};
    if (variant != MobitexBeesat1) {
        std::memcpy(csBytes, a + 3, 8);
        auto ok = [&](const uint8_t* b) { return crc16Xmodem(b, 6) == (((uint16_t)b[6] << 8) | b[7]); };
        bool found = ok(csBytes);
        int flips = 0;
        for (int i = 0; !found && callsignBits >= 1 && i < 64; i++) {
            csBytes[i >> 3] ^= (uint8_t)(1 << (i & 7));
            if (ok(csBytes)) { found = true; flips = 1; break; }
            for (int j = i + 1; callsignBits >= 2 && j < 64; j++) {
                csBytes[j >> 3] ^= (uint8_t)(1 << (j & 7));
                if (ok(csBytes)) { found = true; flips = 2; break; }
                csBytes[j >> 3] ^= (uint8_t)(1 << (j & 7));
            }
            if (found) break;
            csBytes[i >> 3] ^= (uint8_t)(1 << (i & 7));
        }
        if (!found) return false;
        for (int i = 0; i < 6; i++) if (csBytes[i] < 0x20 || csBytes[i] > 0x7E) return false;
        f.bitErrors += flips;
        f.callsign.assign((const char*)csBytes, 6);
        while (!f.callsign.empty() && f.callsign.back() == ' ') f.callsign.pop_back();
    }
    const size_t have = std::min(nb, (n - hl) / kMobBlock);
    if (have < nb) return false;
    MobLfsr lf;
    int okBlocks = 0;
    uint32_t errMap = 0;
    for (size_t b = 0; b < nb; b++) {
        std::vector<uint8_t> x = bitsOf(a + hl + b * kMobBlock, kMobBlock);
        for (uint8_t& bit : x) bit = (uint8_t)(bit ^ lf.next());
        std::vector<uint8_t> y(240);
        for (int i = 0; i < 240; i++) y[(size_t)i] = x[(size_t)mobIdx(i)];
        const std::vector<uint8_t> by = packBits(y, 240);
        std::vector<uint8_t> d;
        for (size_t i = 0; i < kMobBlock; i += 3) {
            const int w0 = (by[i] << 4) | (by[i + 1] >> 4), w1 = ((by[i + 1] & 0xF) << 8) | by[i + 2];
            uint8_t m0, m1;
            const int e0 = mobDecode12(w0, m0), e1 = mobDecode12(w1, m1);
            f.bitErrors += (e0 == 1) + (e1 == 1);
            d.push_back(m0); d.push_back(m1);
        }
        const bool ok = crc16X25(d.data(), 18) == (((uint16_t)d[18] << 8) | d[19]);
        okBlocks += ok;
        if (!ok) errMap |= 1u << b;
        d.resize(18);
        f.blocks.push_back(d);
        f.blockOk.push_back(ok);
    }
    if (okBlocks < 1) return false;
    std::vector<uint8_t>& r = f.reframed;
    r.push_back(c0); r.push_back(c1);
    if (variant != MobitexBeesat1) r.insert(r.end(), csBytes, csBytes + 8);
    for (const auto& d : f.blocks) r.insert(r.end(), d.begin(), d.end());
    r.push_back(0xAA);
    for (int k = 0; k < 4; k++) r.push_back((uint8_t)(errMap >> (8 * k)));
    r.push_back(0xBB);
    return true;
}

// ================================================================ the hunter
namespace {

// One sync word in a stream of soft bits (either polarity); calls back with the `need` soft bits after it (sign corrected).
struct SyncScan {
    uint64_t sync = 0, mask = 0;
    int len = 32, maxErr = 4;
    size_t need = 0;
    std::vector<float> ring;
    uint64_t pos = 0, sh = 0;
    struct Pend { uint64_t start; bool inv; size_t need; bool checked; };
    std::deque<Pend> pend;
    std::function<size_t(std::vector<float>&, bool inv, uint64_t start)> done;   // the bits the frame took (0: none; the syncs inside it are skipped)
    // optional: after earlyBits, the frame's real length in bits from its header (0: not a frame, dropped at once)
    size_t earlyBits = 0;
    std::function<size_t(const std::vector<float>&)> early;
    uint64_t skipUntil = 0;
    uint64_t syncs = 0;
    void init(uint64_t s, int l, int e, size_t n) {
        sync = s; len = l; maxErr = e; need = n;
        mask = l >= 64 ? ~0ull : ((1ull << l) - 1);
        size_t r = 1; while (r < n + 128) r <<= 1;
        ring.assign(r, 0.f);
        early = nullptr; earlyBits = 0;
        reset();
    }
    void reset() { pos = 0; sh = 0; pend.clear(); skipUntil = 0; }
    bool busy() const { for (const Pend& p : pend) if (p.checked || !early) return true; return false; }
    std::vector<float> take(const Pend& p, size_t n) const {
        std::vector<float> v(n);
        for (size_t i = 0; i < n; i++) { const float x = ring[(size_t)((p.start + i) & (ring.size() - 1))]; v[i] = p.inv ? -x : x; }
        return v;
    }
    void push(float s) {
        ring[(size_t)(pos & (ring.size() - 1))] = s;
        pos++;
        sh = (sh << 1) | (s > 0 ? 1u : 0u);
        if (pos >= (uint64_t)len && pos >= skipUntil) {
            const int e = popc64((sh ^ sync) & mask);
            if (e <= maxErr || len - e <= maxErr) {
                if (pend.size() >= 4096) pend.pop_front();
                pend.push_back({pos, e > maxErr, need, early == nullptr});
                syncs++;
            }
        }
        for (size_t k = 0; k < pend.size();) {
            Pend& p = pend[k];
            const uint64_t have = pos - p.start;
            if (p.start < skipUntil) { pend.erase(pend.begin() + (ptrdiff_t)k); continue; }
            if (!p.checked && have >= earlyBits) {
                const size_t n = early(take(p, earlyBits));
                if (!n) { pend.erase(pend.begin() + (ptrdiff_t)k); continue; }
                p.need = std::min(n, need);
                p.checked = true;
            }
            if (p.checked && have >= p.need) {
                const Pend q = p;
                pend.erase(pend.begin() + (ptrdiff_t)k);
                std::vector<float> v = take(q, q.need);
                const size_t used = done ? done(v, q.inv, q.start) : 0;
                if (used) skipUntil = q.start + std::min(used, q.need);
                continue;
            }
            k++;
        }
    }
};

std::vector<uint8_t> hardBytes(const std::vector<float>& v) {
    std::vector<uint8_t> b(v.size() / 8, 0);
    for (size_t i = 0; i < b.size() * 8; i++) if (v[i] > 0) b[i >> 3] = (uint8_t)(b[i >> 3] | (0x80 >> (i & 7)));
    return b;
}

} // namespace

struct MoreHunter::Impl {
    MoreHunter* self = nullptr;
    SyncScan u482c, ngham, mob, mobNx, usp;
    SyncScan concatAsm[2];
    ConvStream vit[2];
    float prevSoft = 0;
    uint64_t nbits = 0;
    int diffPrev[2] = {0, 0};
    std::vector<uint8_t> lastData;
    uint64_t lastPos = 0;

    void emit(MoreFrame& f) {
        // the two Viterbi alignments (or both polarities) find the same frame: report it once
        if (f.data == lastData && f.bitPos < lastPos + 600 && f.bitPos + 600 > lastPos) return;
        lastData = f.data; lastPos = f.bitPos;
        self->frames++;
        if (self->onFrame) self->onFrame(f);
    }
};

MoreHunter::MoreHunter() : p_(new Impl) { p_->self = this; configure(MoreConfig()); }
MoreHunter::~MoreHunter() { delete p_; }
bool MoreHunter::busy() const {
    return p_->u482c.busy() || p_->ngham.busy() || p_->usp.busy() || p_->concatAsm[0].busy() || p_->concatAsm[1].busy();
}

void MoreHunter::configure(const MoreConfig& c) {
    cfg_ = c;
    Impl& I = *p_;
    I.u482c.init(kSyncU482c, 32, c.syncErrU482c, (3 + 255) * 8);
    // the length field: a frame without the Reed-Solomon code has no check at all (a random Golay word decodes half the time), so only RS
    // coded U482C frames are taken from the stream (u482cDecode() itself takes them all)
    I.u482c.earlyBits = 24;
    I.u482c.early = [](const std::vector<float>& v) -> size_t {
        uint32_t w = 0;
        for (int i = 0; i < 24; i++) w = (w << 1) | (v[(size_t)i] > 0 ? 1u : 0u);
        const int lf = golayDecode(w);
        if (lf < 0 || !(lf & kU482cRs) || (lf & 0xFF) <= 32) return 0;
        return (size_t)(3 + (lf & 0xFF)) * 8;
    };
    I.u482c.done = [this](std::vector<float>& v, bool inv, uint64_t st) {
        std::vector<uint8_t> b = hardBytes(v), pk;
        int rs = -1, fl = 0;
        if (!u482cDecode(b.data(), b.size(), pk, rs, fl)) { failed++; return (size_t)0; }
        MoreFrame f; f.framing = U482c; f.name = "U482C"; f.data = pk; f.rsErrors = rs; f.inverted = inv; f.bitPos = st;
        f.how = std::string("Golay") + ((fl & kU482cConv) ? " + Viterbi" : "") + ((fl & kU482cRs) ? " + Reed-Solomon" : "");
        p_->emit(f);
        const int lf = golayDecode(((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2]);
        return (size_t)(3 + (lf & 0xFF)) * 8;
    };
    I.ngham.init(kSyncNgham, 32, c.syncErrNgham, (3 + 255) * 8);
    I.ngham.earlyBits = 24;
    I.ngham.early = [](const std::vector<float>& v) -> size_t {
        uint32_t w = 0;
        for (int i = 0; i < 24; i++) w = (w << 1) | (v[(size_t)i] > 0 ? 1u : 0u);
        for (int i = 0; i < 7; i++) if (__builtin_popcount(w ^ kNghTag[i]) <= 6) return (size_t)(3 + kNghCw[i]) * 8;
        return 0;
    };
    I.ngham.done = [this](std::vector<float>& v, bool inv, uint64_t st) {
        std::vector<uint8_t> b = hardBytes(v), pl;
        int rs = -1;
        if (!nghamDecode(b.data(), b.size(), pl, rs)) { failed++; return (size_t)0; }
        MoreFrame f; f.framing = Ngham; f.name = "NGHam"; f.data = pl; f.rsErrors = rs; f.inverted = inv; f.bitPos = st;
        f.how = rs > 0 ? "Reed-Solomon + CRC-16" : "CRC-16";
        p_->emit(f);
        const uint32_t tag = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
        int best = 0, bd = 99;
        for (int i = 0; i < 7; i++) { const int d = __builtin_popcount(tag ^ kNghTag[i]); if (d < bd) { bd = d; best = i; } }
        return (size_t)(3 + kNghCw[best]) * 8;
    };
    auto mobDone = [this](bool nx) {
        return [this, nx](std::vector<float>& v, bool inv, uint64_t st) {
            std::vector<uint8_t> b = hardBytes(v);
            MobitexFrame mf;
            int variant = nx ? cfg_.mobitexVariant : MobitexDefault;
            bool ok = mobitexDecode(b.data(), b.size(), variant, mf);
            if (!ok && nx && variant == MobitexDefault) { variant = MobitexBeesat1; ok = mobitexDecode(b.data(), b.size(), variant, mf); }
            if (!ok) { failed++; return (size_t)0; }
            MoreFrame f; f.framing = Mobitex; f.inverted = inv; f.bitPos = st; f.data = mf.reframed; f.callsign = mf.callsign;
            f.name = variant == MobitexBeesat1 ? "BEESAT-1" : variant == MobitexBeesat9 ? "BEESAT-9" : nx ? "Mobitex-NX" : "Mobitex";
            int okb = 0; for (bool k : mf.blockOk) okb += k;
            f.how = "FEC + CRC-16 (" + std::to_string(okb) + " of " + std::to_string(mf.blockOk.size()) + " blocks)";
            f.rsErrors = -1;
            p_->emit(f);
            return (mobHeader(variant) + mf.blocks.size() * kMobBlock) * 8;
        };
    };
    const size_t mobNeed = mobitexMaxBytes(c.mobitexVariant == MobitexBeesat1 ? MobitexDefault : c.mobitexVariant) * 8;
    I.mob.init(kSyncMobitex, 16, c.syncErrMobitex, mobNeed);
    I.mob.done = mobDone(false);
    I.mobNx.init(kSyncMobitexNx, 16, c.syncErrMobitex, mobNeed);
    I.mobNx.done = mobDone(true);
    // the control bytes (and, for plain Mobitex, the callsign CRC) right after the 16 bit sync word: most false syncs end here
    auto mobEarly = [](int variant, bool checkCall) {
        return [variant, checkCall](const std::vector<float>& v) -> size_t {
            std::vector<uint8_t> b = hardBytes(v);
            uint8_t c0, c1;
            if (mobDecode12((b[0] << 4) | (b[2] >> 4), c0) == 2 || mobDecode12((b[1] << 4) | (b[2] & 0xF), c1) == 2) return 0;
            if (checkCall) {
                const uint8_t* cs = b.data() + 3;
                bool ok = false;
                uint8_t t[8];
                std::memcpy(t, cs, 8);
                auto good = [&]() { return crc16Xmodem(t, 6) == (((uint16_t)t[6] << 8) | t[7]); };
                ok = good();
                for (int i = 0; !ok && i < 64; i++) {
                    t[i >> 3] ^= (uint8_t)(1 << (i & 7));
                    if (good()) { ok = true; break; }
                    for (int j = i + 1; j < 64; j++) { t[j >> 3] ^= (uint8_t)(1 << (j & 7)); if (good()) { ok = true; break; } t[j >> 3] ^= (uint8_t)(1 << (j & 7)); }
                    if (ok) break;
                    t[i >> 3] ^= (uint8_t)(1 << (i & 7));
                }
                if (!ok) return 0;
            }
            const size_t nb = variant == MobitexBeesat9 ? 32 : (size_t)(c0 & 0x1F) + 1;
            return (mobHeader(variant == MobitexBeesat1 ? MobitexBeesat1 : MobitexDefault) + nb * kMobBlock) * 8;
        };
    };
    I.mob.earlyBits = 11 * 8;
    I.mob.early = mobEarly(MobitexDefault, true);
    I.mobNx.earlyBits = 11 * 8;
    // NX: the configured variant; the default one also falls back to BEESAT-1 (no callsign), so the longer header is waited for
    I.mobNx.early = mobEarly(c.mobitexVariant == MobitexBeesat1 ? MobitexBeesat1 : c.mobitexVariant, c.mobitexVariant != MobitexDefault && c.mobitexVariant != MobitexBeesat1);
    uint64_t us = 0;
    for (int k = 0; k < 8; k++) us = (us << 8) | kUspSync[k];
    I.usp.init(us, 64, c.syncErrUsp, (size_t)kUspPlsBits + kUspMaxSymbols);
    I.usp.done = [this](std::vector<float>& v, bool inv, uint64_t st) {
        std::vector<uint8_t> ax;
        int rs = -1, dl = 0;
        if (!uspDecode(v.data(), v.size(), ax, rs, &dl)) { failed++; return (size_t)0; }
        MoreFrame f; f.framing = Usp; f.name = "USP"; f.data = ax; f.rsErrors = rs; f.inverted = inv; f.bitPos = st;
        f.how = "Viterbi + Reed-Solomon (" + std::to_string(dl) + " byte block)";
        p_->emit(f);
        return (size_t)(64 + (dl + 32) * 16);
    };
    for (int k = 0; k < 2; k++) {
        I.vit[k].setKind(c.concatParams.conv);
        I.concatAsm[k].init(kAsmCcsds, 32, c.syncErrConcat, (size_t)c.concatParams.blockBytes() * 8);
        I.concatAsm[k].done = [this](std::vector<float>& v, bool inv, uint64_t st) {
            std::vector<uint8_t> b = hardBytes(v), fr;
            int rs = -1;
            if (!ccsdsConcatDecode(b.data(), cfg_.concatParams, fr, rs)) { failed++; return (size_t)0; }
            MoreFrame f; f.framing = CcsdsConcat; f.name = "CCSDS concatenated"; f.data = fr; f.rsErrors = rs; f.inverted = inv; f.bitPos = st * 2;
            f.how = "Viterbi + Reed-Solomon";
            p_->emit(f);
            return v.size();
        };
    }
    reset();
}

void MoreHunter::reset() {
    Impl& I = *p_;
    I.u482c.reset(); I.ngham.reset(); I.mob.reset(); I.mobNx.reset(); I.usp.reset();
    for (int k = 0; k < 2; k++) { I.concatAsm[k].reset(); I.vit[k].reset(); I.diffPrev[k] = 0; }
    I.prevSoft = 0; I.nbits = 0;
    I.lastData.clear();
    syncs = failed = frames = 0;
}

void MoreHunter::push(float s) {
    Impl& I = *p_;
    const float h = s > 0 ? 1.f : -1.f;
    if (cfg_.u482c) I.u482c.push(h);
    if (cfg_.ngham) I.ngham.push(h);
    if (cfg_.mobitex) { I.mob.push(h); I.mobNx.push(h); }
    if (cfg_.usp) I.usp.push(s);
    if (cfg_.concat) {
        // two symbol pair alignments: (2k, 2k + 1) and (2k + 1, 2k + 2)
        const int k = (int)(I.nbits & 1u);
        I.vit[k].push(I.prevSoft, s);
        std::vector<uint8_t>& o = I.vit[k].out();
        for (uint8_t b : o) {
            int bit = b;
            if (cfg_.concatParams.differential) { const int d = bit ^ I.diffPrev[k]; I.diffPrev[k] = bit; bit = d; }
            I.concatAsm[k].push(bit ? 1.f : -1.f);
        }
        o.clear();
    }
    I.prevSoft = s;
    I.nbits++;
    syncs = I.u482c.syncs + I.ngham.syncs + I.mob.syncs + I.mobNx.syncs + I.usp.syncs + I.concatAsm[0].syncs + I.concatAsm[1].syncs;
}

} // namespace cubesat
} // namespace dect2
