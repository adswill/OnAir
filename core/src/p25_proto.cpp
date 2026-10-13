// P25 Phase 1 protocol pieces (see p25_proto.h).
#include "dect2/p25_proto.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace dect2 {
namespace p25 {

namespace {
int popc(uint64_t v) { return __builtin_popcountll(v); }

// ------------------------------------------------------------ GF(64), x^6 + x + 1
struct Gf {
    uint8_t exp[128], log[64];
    Gf() {
        int x = 1;
        for (int i = 0; i < 63; i++) { exp[i] = (uint8_t)x; log[x] = (uint8_t)i; x <<= 1; if (x & 64) x ^= 0x43; }
        for (int i = 63; i < 128; i++) exp[i] = exp[i - 63];
        log[0] = 0;
    }
    uint8_t mul(int a, int b) const { return a && b ? exp[log[a] + log[b]] : 0; }
    uint8_t div(int a, int b) const { return a ? exp[(log[a] + 63 - log[b]) % 63] : 0; }
    uint8_t pw(int i) const { return exp[((i % 63) + 63) % 63]; }
};
const Gf& gf() { static const Gf g; return g; }

// ------------------------------------------------------------ NID: BCH(63,16) generator 6331 1413 6723 5453 (octal)
constexpr uint64_t kBchGen = 06331141367235453ULL;
uint64_t bchCodeword(int m) {   // bit 63 sent first: 16 data bits, 47 parity bits, the overall parity
    uint64_t r = (uint64_t)m << 47;
    for (int i = 62; i >= 47; i--) if (r >> i & 1) r ^= kBchGen << (i - 47);
    const uint64_t c63 = ((uint64_t)m << 47) | r;
    return (c63 << 1) | (uint64_t)(popc(c63) & 1);
}
const std::vector<uint64_t>& nidTable() {
    static std::vector<uint64_t> t;
    static std::once_flag f;
    std::call_once(f, [] { t.resize(65536); for (int m = 0; m < 65536; m++) t[(size_t)m] = bchCodeword(m); });
    return t;
}

// ------------------------------------------------------------ Golay(23,12) generator x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1
constexpr uint32_t kGolayGen = 0xC75;
const std::vector<uint32_t>& golayTable() {
    static std::vector<uint32_t> t;
    static std::once_flag f;
    std::call_once(f, [] { t.resize(4096); for (int d = 0; d < 4096; d++) t[(size_t)d] = golay24Encode(d); });
    return t;
}

const int kHamPar[6] = {0xE, 0xD, 0xB, 0x7, 0x3, 0xC};

// ------------------------------------------------------------ trellis
const uint8_t kNext[4][4] = {{0x2, 0xC, 0x1, 0xF}, {0xE, 0x0, 0xD, 0x3}, {0x9, 0x7, 0xA, 0x4}, {0x5, 0xB, 0x6, 0x8}};
struct Il { int t[98]; Il() { int k = 0; for (int c = 0; c < 4; c++) for (int i = 2 * c; i < 98; i += 8) { t[k++] = i; t[k++] = i + 1; } } };
const Il& il() { static const Il i; return i; }

// ------------------------------------------------------------ IMBE interleave (TIA-102.BABA), as in DSD
const int iW[72] = {0, 2, 4, 1, 3, 5, 0, 2, 4, 1, 3, 6, 0, 2, 4, 1, 3, 6, 0, 2, 4, 1, 3, 6, 0, 2, 4, 1, 3, 6, 0, 2, 4, 1, 3, 6,
                    0, 2, 5, 1, 3, 6, 0, 2, 5, 1, 3, 6, 0, 2, 5, 1, 3, 7, 0, 2, 5, 1, 3, 7, 0, 2, 5, 1, 4, 7, 0, 3, 5, 2, 4, 7};
const int iX[72] = {22, 20, 10, 20, 18, 0, 20, 18, 8, 18, 16, 13, 18, 16, 6, 16, 14, 11, 16, 14, 4, 14, 12, 9, 14, 12, 2, 12, 10, 7, 12, 10, 0, 10, 8, 5,
                    10, 8, 13, 8, 6, 3, 8, 6, 11, 6, 4, 1, 6, 4, 9, 4, 2, 6, 4, 2, 7, 2, 0, 4, 2, 0, 5, 0, 13, 2, 0, 21, 3, 21, 11, 0};
const int iY[72] = {1, 3, 5, 0, 2, 4, 1, 3, 6, 0, 2, 4, 1, 3, 6, 0, 2, 4, 1, 3, 6, 0, 2, 4, 1, 3, 6, 0, 2, 4, 1, 3, 6, 0, 2, 5,
                    1, 3, 6, 0, 2, 5, 1, 3, 6, 0, 2, 5, 1, 3, 6, 0, 2, 5, 1, 3, 7, 0, 2, 5, 1, 4, 7, 0, 3, 5, 2, 4, 7, 1, 3, 5};
const int iZ[72] = {21, 19, 1, 21, 19, 9, 19, 17, 14, 19, 17, 7, 17, 15, 12, 17, 15, 5, 15, 13, 10, 15, 13, 3, 13, 11, 8, 13, 11, 1, 11, 9, 6, 11, 9, 14,
                    9, 7, 4, 9, 7, 12, 7, 5, 2, 7, 5, 10, 5, 3, 0, 5, 3, 8, 3, 1, 5, 3, 1, 6, 1, 14, 3, 1, 22, 4, 22, 12, 1, 22, 20, 2};
const int kImbeLen[8] = {23, 23, 23, 23, 15, 15, 15, 7};

void putBits(uint8_t* b, int& p, uint64_t v, int n) { for (int i = n - 1; i >= 0; i--) b[p++] = (uint8_t)((v >> i) & 1); }
uint64_t getBits(const uint8_t* b, int& p, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = (v << 1) | b[p++]; return v; }
void bitsToHex(const uint8_t* b, int nHex, uint8_t* hex) { for (int i = 0; i < nHex; i++) { int v = 0; for (int k = 0; k < 6; k++) v = (v << 1) | b[6 * i + k]; hex[i] = (uint8_t)v; } }
void hexToBits(const uint8_t* hex, int nHex, uint8_t* b) { for (int i = 0; i < nHex; i++) for (int k = 0; k < 6; k++) b[6 * i + k] = (uint8_t)((hex[i] >> (5 - k)) & 1); }
} // namespace

const int kSync[kSyncSym] = {3, 3, 3, 3, 3, -3, 3, 3, -3, -3, 3, 3, -3, -3, -3, -3, 3, -3, 3, -3, -3, -3, -3, -3};

const char* duidName(int d) {
    switch (d) {
    case kHdu: return "HDU"; case kTdu: return "TDU"; case kLdu1: return "LDU1"; case kTsbk: return "TSBK";
    case kLdu2: return "LDU2"; case kPdu: return "PDU"; case kTdulc: return "TDULC"; default: return "?";
    }
}
int frameLen(int d) {
    switch (d) {
    case kHdu: return 396; case kTdu: return 72; case kLdu1: case kLdu2: return 864; case kTdulc: return 216;
    case kTsbk: case kPdu: return 360; default: return 0;
    }
}

void nidEncode(int nac, int duid, uint8_t bits[64]) {
    const uint64_t c = bchCodeword(((nac & 0xFFF) << 4) | (duid & 0xF));
    for (int i = 0; i < 64; i++) bits[i] = (uint8_t)((c >> (63 - i)) & 1);
}
bool nidDecode(const uint8_t bits[64], int& nac, int& duid, int* errs) {
    uint64_t x = 0;
    for (int i = 0; i < 64; i++) x = (x << 1) | (bits[i] & 1);
    const auto& t = nidTable();
    int best = 99, bm = 0;
    for (int m = 0; m < 65536; m++) { const int d = popc((t[(size_t)m] ^ x) >> 1); if (d < best) { best = d; bm = m; if (d == 0) break; } }
    if (errs) *errs = best;
    if (best > 10) return false;
    nac = bm >> 4; duid = bm & 0xF;
    return true;
}

void rsEncode(uint8_t* sym, int n, int k) {
    const Gf& g = gf();
    const int np = n - k;
    std::vector<uint8_t> gp((size_t)np + 1, 0);   // descending powers, gp[0] = 1
    gp[0] = 1;
    for (int i = 1; i <= np; i++) {   // multiply by (x + a^i)
        const uint8_t a = g.pw(i);
        for (int j = i; j >= 1; j--) gp[(size_t)j] ^= g.mul(gp[(size_t)j - 1], a);
    }
    std::vector<uint8_t> r((size_t)n, 0);
    for (int i = 0; i < k; i++) r[(size_t)i] = sym[i] & 63;
    for (int i = 0; i < k; i++) {
        const uint8_t c = r[(size_t)i];
        if (c) for (int j = 1; j <= np; j++) r[(size_t)(i + j)] ^= g.mul(c, gp[(size_t)j]);
    }
    for (int i = k; i < n; i++) sym[i] = r[(size_t)i];
}

bool rsDecode(uint8_t* sym, int n, int k, int* errs) {
    const Gf& g = gf();
    const int np = n - k;
    std::vector<uint8_t> S((size_t)np, 0);
    bool any = false;
    for (int i = 1; i <= np; i++) {
        uint8_t s = 0;
        for (int t = 0; t < n; t++) if (sym[t] & 63) s ^= g.mul(sym[t] & 63, g.pw(i * (n - 1 - t)));
        S[(size_t)i - 1] = s;
        if (s) any = true;
    }
    if (errs) *errs = 0;
    if (!any) return true;
    // Berlekamp-Massey
    std::vector<uint8_t> C((size_t)np + 1, 0), B((size_t)np + 1, 0), T;
    C[0] = B[0] = 1;
    int L = 0, m = 1; uint8_t b = 1;
    for (int r = 0; r < np; r++) {
        uint8_t d = S[(size_t)r];
        for (int i = 1; i <= L; i++) d ^= g.mul(C[(size_t)i], S[(size_t)(r - i)]);
        if (d == 0) { m++; continue; }
        T = C;
        const uint8_t coef = g.div(d, b);
        for (int i = 0; i + m <= np; i++) C[(size_t)(i + m)] ^= g.mul(coef, B[(size_t)i]);
        if (2 * L <= r) { L = r + 1 - L; B = T; b = d; m = 1; } else m++;
    }
    if (2 * L > np) return false;
    // Chien search over the n positions (power j = n - 1 - t)
    std::vector<int> pos;
    for (int j = 0; j < n; j++) {
        uint8_t v = 0;
        for (int i = 0; i <= L; i++) v ^= g.mul(C[(size_t)i], g.pw(-j * i));
        if (v == 0) pos.push_back(j);
    }
    if ((int)pos.size() != L) return false;
    // Forney (first root a^1): e = Omega(X^-1) / Lambda'(X^-1)
    std::vector<uint8_t> Om((size_t)np, 0);
    for (int i = 0; i < np; i++) for (int j = 0; j <= std::min(i, L); j++) Om[(size_t)i] ^= g.mul(C[(size_t)j], S[(size_t)(i - j)]);
    for (int j : pos) {
        const uint8_t xi = g.pw(-j);
        uint8_t om = 0, dl = 0;
        for (int i = 0; i < np; i++) om ^= g.mul(Om[(size_t)i], g.pw(-j * i));
        for (int i = 1; i <= L; i += 2) dl ^= g.mul(C[(size_t)i], g.pw(-j * (i - 1)));
        (void)xi;
        if (!dl) return false;
        const uint8_t e = g.div(om, dl);
        sym[n - 1 - j] = (uint8_t)((sym[n - 1 - j] ^ e) & 63);
    }
    if (errs) *errs = L;
    // check
    for (int i = 1; i <= np; i++) {
        uint8_t s = 0;
        for (int t = 0; t < n; t++) if (sym[t]) s ^= g.mul(sym[t], g.pw(i * (n - 1 - t)));
        if (s) return false;
    }
    return true;
}

uint16_t hamming10Encode(int hex) {
    int par = 0;
    for (int i = 0; i < 6; i++) if (hex >> (5 - i) & 1) par ^= kHamPar[i];
    return (uint16_t)(((hex & 63) << 4) | par);
}
int hamming10Decode(uint16_t cw, int* errs) {
    int best = 99, bh = 0;
    for (int h = 0; h < 64; h++) { const int d = popc((uint64_t)(hamming10Encode(h) ^ (cw & 0x3FF))); if (d < best) { best = d; bh = h; } }
    if (errs) *errs = best;
    return bh;
}

uint32_t golay24Encode(int d12) {
    uint32_t r = (uint32_t)(d12 & 0xFFF) << 11;
    for (int i = 22; i >= 11; i--) if (r >> i & 1) r ^= kGolayGen << (i - 11);
    const uint32_t c23 = ((uint32_t)(d12 & 0xFFF) << 11) | r;
    return (c23 << 1) | (uint32_t)(popc(c23) & 1);
}
int golay24Decode(uint32_t cw, int* errs) {
    const auto& t = golayTable();
    int best = 99, bd = 0;
    for (int d = 0; d < 4096; d++) { const int e = popc((uint64_t)(t[(size_t)d] ^ (cw & 0xFFFFFF))); if (e < best) { best = e; bd = d; if (!e) break; } }
    if (errs) *errs = best;
    return best <= 3 ? bd : -1;
}
uint32_t golay18Encode(int hex) { return golay24Encode(hex & 63) & 0x3FFFF; }
int golay18Decode(uint32_t cw, int* errs) {
    int best = 99, bh = 0;
    for (int h = 0; h < 64; h++) { const int e = popc((uint64_t)(golay18Encode(h) ^ (cw & 0x3FFFF))); if (e < best) { best = e; bh = h; } }
    if (errs) *errs = best;
    return best <= 3 ? bh : -1;
}

uint16_t crcCcitt(const uint8_t* bits, int n) {
    uint16_t c = 0;
    for (int i = 0; i < n; i++) {
        const bool fb = ((c >> 15) & 1) ^ (bits[i] & 1);
        c = (uint16_t)(c << 1);
        if (fb) c ^= 0x1021;
    }
    return (uint16_t)~c;
}

void trellisEncode(const uint8_t bits[96], uint8_t dibits[98]) {
    uint8_t coded[98];
    int st = 0;
    for (int i = 0; i < 49; i++) {
        const int in = i < 48 ? (bits[2 * i] << 1) | bits[2 * i + 1] : 0;
        const int o = kNext[st][in];
        coded[2 * i] = (uint8_t)(o >> 2); coded[2 * i + 1] = (uint8_t)(o & 3);
        st = in;
    }
    for (int i = 0; i < 98; i++) dibits[i] = coded[il().t[i]];
}

bool trellisDecode(const float lv[98], uint8_t bits[96], float* metric) {
    float r[98];
    for (int i = 0; i < 98; i++) r[il().t[i]] = lv[i];
    float pm[4] = {0, 1e9f, 1e9f, 1e9f};
    uint8_t from[49][4];
    for (int s = 0; s < 49; s++) {
        float nm[4] = {1e30f, 1e30f, 1e30f, 1e30f};
        for (int st = 0; st < 4; st++) {
            if (pm[st] >= 1e29f) continue;
            for (int in = 0; in < 4; in++) {
                if (s == 48 && in != 0) continue;
                const int o = kNext[st][in];
                const float a = r[2 * s] - dibitToSym(o >> 2), b = r[2 * s + 1] - dibitToSym(o & 3);
                const float m = pm[st] + a * a + b * b;
                if (m < nm[in]) { nm[in] = m; from[s][in] = (uint8_t)st; }
            }
        }
        memcpy(pm, nm, sizeof pm);
    }
    int st = 0;
    uint8_t in[49];
    for (int s = 48; s >= 0; s--) { in[s] = (uint8_t)st; st = from[s][st]; }
    for (int i = 0; i < 48; i++) { bits[2 * i] = (uint8_t)(in[i] >> 1); bits[2 * i + 1] = (uint8_t)(in[i] & 1); }
    if (metric) *metric = pm[0];
    return pm[0] < 1e29f;
}

void tsbkBits(const Tsbk& t, uint8_t b[96]) {
    int p = 0;
    putBits(b, p, t.last, 1); putBits(b, p, t.prot, 1); putBits(b, p, (uint64_t)t.opcode, 6); putBits(b, p, (uint64_t)t.mfid, 8); putBits(b, p, t.args, 64);
    putBits(b, p, crcCcitt(b, 80), 16);
}
bool tsbkParse(const uint8_t b[96], Tsbk& t) {
    int p = 0;
    t.last = getBits(b, p, 1); t.prot = getBits(b, p, 1); t.opcode = (int)getBits(b, p, 6); t.mfid = (int)getBits(b, p, 8); t.args = getBits(b, p, 64);
    return (uint16_t)getBits(b, p, 16) == crcCcitt(b, 80);
}
const char* opcodeName(int op) {
    switch (op) {
    case kGrpVGrant: return "GRP_V_CH_GRANT"; case kGrpVGrantUpd: return "GRP_V_CH_GRANT_UPDT"; case kUuVGrant: return "UU_V_CH_GRANT";
    case kIdenUpVu: return "IDEN_UP_VU"; case kRfssSts: return "RFSS_STS_BCST"; case kNetSts: return "NET_STS_BCST"; case kAdjSts: return "ADJ_STS_BCST";
    case kIdenUp: return "IDEN_UP"; default: return "TSBK";
    }
}

void lcHexbits(const Lc& l, uint8_t hex[12]) {
    uint8_t b[72] = {}; int p = 0;
    putBits(b, p, l.prot, 1); putBits(b, p, 0, 1); putBits(b, p, (uint64_t)l.lco, 6); putBits(b, p, (uint64_t)l.mfid, 8);
    if (l.lco == kLcGroup) { putBits(b, p, (uint64_t)l.svcOpts, 8); putBits(b, p, 0, 8); putBits(b, p, (uint64_t)l.tg, 16); putBits(b, p, (uint64_t)l.src, 24); }
    else if (l.lco == kLcUnit) { putBits(b, p, (uint64_t)l.svcOpts, 8); putBits(b, p, (uint64_t)l.dst, 24); putBits(b, p, (uint64_t)l.src, 24); }
    else if (l.lco == kLcTerm) { putBits(b, p, 0, 32); putBits(b, p, (uint64_t)(l.dst < 0 ? 0 : l.dst), 24); }
    else putBits(b, p, l.raw & 0xFFFFFFFFFFFFFFULL, 56);
    bitsToHex(b, 12, hex);
}
bool lcParse(const uint8_t hex[12], Lc& l) {
    uint8_t b[72]; hexToBits(hex, 12, b);
    int p = 0;
    l = Lc();
    l.prot = getBits(b, p, 1); getBits(b, p, 1); l.lco = (int)getBits(b, p, 6); l.mfid = (int)getBits(b, p, 8);
    int q = p; l.raw = getBits(b, q, 56);
    if (l.mfid != 0 && l.mfid != 1) return true;   // a manufacturer's own: kept raw
    if (l.lco == kLcGroup) { l.svcOpts = (int)getBits(b, p, 8); getBits(b, p, 8); l.tg = (int)getBits(b, p, 16); l.src = (int)getBits(b, p, 24); }
    else if (l.lco == kLcUnit) { l.svcOpts = (int)getBits(b, p, 8); l.dst = (int)getBits(b, p, 24); l.src = (int)getBits(b, p, 24); }
    else if (l.lco == kLcTerm) { getBits(b, p, 32); l.dst = (int)getBits(b, p, 24); }
    return true;
}

const char* algName(int a) {
    switch (a) {
    case 0x80: return "clear"; case 0x81: return "DES-OFB"; case 0x83: return "triple DES"; case 0x84: return "AES-256";
    case 0x85: return "AES-128"; case 0x9F: return "DES-XL"; case 0xAA: return "ADP (RC4)"; default: return "unknown algorithm";
    }
}
void esHexbits(const Crypto& c, uint8_t hex[16]) {
    uint8_t b[96]; int p = 0;
    for (int i = 0; i < 9; i++) putBits(b, p, c.mi[i], 8);
    putBits(b, p, (uint64_t)c.algId, 8); putBits(b, p, (uint64_t)c.keyId, 16);
    bitsToHex(b, 16, hex);
}
void esParse(const uint8_t hex[16], Crypto& c) {
    uint8_t b[96]; hexToBits(hex, 16, b); int p = 0;
    for (int i = 0; i < 9; i++) c.mi[i] = (uint8_t)getBits(b, p, 8);
    c.algId = (int)getBits(b, p, 8); c.keyId = (int)getBits(b, p, 16);
}
void hduHexbits(const Crypto& c, uint8_t hex[20]) {
    uint8_t b[120]; int p = 0;
    for (int i = 0; i < 9; i++) putBits(b, p, c.mi[i], 8);
    putBits(b, p, (uint64_t)c.mfid, 8); putBits(b, p, (uint64_t)c.algId, 8); putBits(b, p, (uint64_t)c.keyId, 16); putBits(b, p, (uint64_t)(c.tg < 0 ? 0 : c.tg), 16);
    bitsToHex(b, 20, hex);
}
void hduParse(const uint8_t hex[20], Crypto& c) {
    uint8_t b[120]; hexToBits(hex, 20, b); int p = 0;
    for (int i = 0; i < 9; i++) c.mi[i] = (uint8_t)getBits(b, p, 8);
    c.mfid = (int)getBits(b, p, 8); c.algId = (int)getBits(b, p, 8); c.keyId = (int)getBits(b, p, 16); c.tg = (int)getBits(b, p, 16);
}

void imbeDeinterleave(const uint8_t d[72], char fr[8][23]) {
    memset(fr, 0, 8 * 23);
    for (int i = 0; i < 72; i++) { fr[iW[i]][iX[i]] = (char)((d[i] >> 1) & 1); fr[iY[i]][iZ[i]] = (char)(d[i] & 1); }
}
void imbeInterleave(const char fr[8][23], uint8_t d[72]) {
    for (int i = 0; i < 72; i++) d[i] = (uint8_t)(((fr[iW[i]][iX[i]] & 1) << 1) | (fr[iY[i]][iZ[i]] & 1));
}
void imbeFlatten(const char fr[8][23], uint8_t bits[144]) {
    int p = 0;
    for (int r = 0; r < 8; r++) for (int j = 0; j < kImbeLen[r]; j++) bits[p++] = (uint8_t)(fr[r][j] & 1);
}
void imbeUnflatten(const uint8_t bits[144], char fr[8][23]) {
    memset(fr, 0, 8 * 23);
    int p = 0;
    for (int r = 0; r < 8; r++) for (int j = 0; j < kImbeLen[r]; j++) fr[r][j] = (char)(bits[p++] & 1);
}

std::string channelText(int ch) {
    char b[24];
    snprintf(b, sizeof b, "%d-%d", (ch >> 12) & 15, ch & 0xFFF);
    return b;
}

} // namespace p25
} // namespace dect2
