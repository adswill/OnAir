// CubeSat framings: see cubesat_proto.h.
#include "dect2/cubesat/cubesat_proto.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include "dect2/common/bits.h"

namespace dect2 {
namespace cubesat {

const char* framingName(int f) {
    switch (f) {
    case Ax25Afsk1200: return "AX.25 AFSK 1200";
    case Ax25G3ruh: return "AX.25 G3RUH FSK";
    case Ax100Mode5: return "AX100 Mode 5 (ASM + Golay)";
    case Ax100Mode6: return "AX100 Mode 6 (HDLC + RS)";
    case CcsdsRs: return "CCSDS RS (255,223)";
    case U482c: return "GOMspace U482C";
    case CcsdsConcat: return "CCSDS concatenated (Viterbi + RS)";
    case Usp: return "USP (SPUTNIX)";
    case Ngham: return "NGHam";
    case Mobitex: return "Mobitex / BEESAT";
    default: return "?";
    }
}
const char* framingShort(int f) {
    switch (f) {
    case Ax25Afsk1200: return "AFSK";
    case Ax25G3ruh: return "G3RUH";
    case Ax100Mode5: return "AX100 M5";
    case Ax100Mode6: return "AX100 M6";
    case CcsdsRs: return "CCSDS";
    case U482c: return "U482C";
    case CcsdsConcat: return "CCSDS conc";
    case Usp: return "USP";
    case Ngham: return "NGHam";
    case Mobitex: return "Mobitex";
    default: return "?";
    }
}

// ---------------------------------------------------------------- Reed-Solomon

namespace {

struct Gf {
    uint8_t exp[512], log[256];
    uint8_t tal[256], tal1[256];      // conventional -> dual basis, and back
    uint8_t gen[kRsParity + 1];      // the generator polynomial, highest power first: gen[0] = 1
    Gf() {
        int x = 1;
        for (int i = 0; i < 255; i++) {
            exp[i] = (uint8_t)x; log[x] = (uint8_t)i;
            x <<= 1;
            if (x & 0x100) x ^= 0x187;
        }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
        log[0] = 0;
        // g(x) = prod (x - beta^(112 + i)), beta = alpha^11
        std::vector<uint8_t> g(1, 1);
        for (int i = 0; i < kRsParity; i++) {
            const uint8_t r = exp[(11 * (112 + i)) % 255];
            std::vector<uint8_t> n(g.size() + 1, 0);
            for (size_t j = 0; j < g.size(); j++) { n[j] ^= g[j]; n[j + 1] ^= mul(g[j], r); }
            g = n;
        }
        for (int i = 0; i <= kRsParity; i++) gen[i] = g[(size_t)i];
        static const uint8_t t[8] = {0x8d, 0xef, 0xec, 0x86, 0xfa, 0x99, 0xaf, 0x7b};
        for (int i = 0; i < 256; i++) {
            uint8_t v = 0;
            for (int j = 0; j < 8; j++) if (i & (1 << j)) v ^= t[7 - j];
            tal[i] = v;
            tal1[v] = (uint8_t)i;
        }
    }
    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? exp[log[a] + log[b]] : 0; }
    uint8_t div(uint8_t a, uint8_t b) const { return a ? exp[(log[a] + 255 - log[b]) % 255] : 0; }
    uint8_t pw(int e) const { e %= 255; if (e < 0) e += 255; return exp[e]; }
};
const Gf& gf() { static const Gf g; return g; }

} // namespace

void rsParity(const uint8_t* data, size_t n, uint8_t* par) {
    const Gf& G = gf();
    uint8_t r[kRsParity] = {};
    for (size_t i = 0; i < n; i++) {
        const uint8_t fb = data[i] ^ r[0];
        std::memmove(r, r + 1, kRsParity - 1);
        r[kRsParity - 1] = 0;
        if (fb) for (int j = 0; j < kRsParity; j++) r[j] ^= G.mul(fb, G.gen[j + 1]);
    }
    std::memcpy(par, r, kRsParity);
}

int rsDecode(uint8_t* c, size_t n) {
    if (n <= (size_t)kRsParity || n > 255) return -1;
    const Gf& G = gf();
    // syndromes S_i = c(beta^(112 + i)), byte j is the coefficient of x^(n-1-j)
    uint8_t S[kRsParity];
    bool any = false;
    for (int i = 0; i < kRsParity; i++) {
        const uint8_t r = G.pw(11 * (112 + i));
        uint8_t s = 0;
        for (size_t j = 0; j < n; j++) s = G.mul(s, r) ^ c[j];
        S[i] = s;
        any |= s != 0;
    }
    if (!any) return 0;
    // Berlekamp-Massey
    uint8_t C[kRsParity + 1] = {1}, B[kRsParity + 1] = {1};
    int L = 0, m = 1;
    uint8_t b = 1;
    for (int k = 0; k < kRsParity; k++) {
        uint8_t d = S[k];
        for (int i = 1; i <= L; i++) d ^= G.mul(C[i], S[k - i]);
        if (d == 0) { m++; continue; }
        uint8_t T[kRsParity + 1];
        std::memcpy(T, C, sizeof C);
        const uint8_t coef = G.div(d, b);
        for (int i = 0; i + m <= kRsParity; i++) C[i + m] ^= G.mul(coef, B[i]);
        if (2 * L <= k) { L = k + 1 - L; std::memcpy(B, T, sizeof B); b = d; m = 1; }
        else m++;
    }
    if (L > kRsParity / 2) return -1;
    // Omega = S * Lambda mod x^32
    uint8_t Om[kRsParity] = {};
    for (int i = 0; i < kRsParity; i++)
        for (int j = 0; j <= i && j <= L; j++) Om[i] ^= G.mul(C[j], S[i - j]);
    // Chien search over the n real positions: X = beta^e, e = n - 1 - j
    int found = 0;
    for (size_t j = 0; j < n; j++) {
        const int e = (int)(n - 1 - j);
        const int xi = (255 - (11 * e) % 255) % 255;          // log of X^-1
        uint8_t v = 0;
        for (int i = L; i >= 0; i--) v = G.mul(v, G.exp[xi]) ^ C[i];
        if (v) continue;
        // Forney: Y = X^(1 - 112) * Omega(X^-1) / Lambda'(X^-1)
        uint8_t om = 0;
        for (int i = kRsParity - 1; i >= 0; i--) om = G.mul(om, G.exp[xi]) ^ Om[i];
        uint8_t dl = 0;
        for (int i = 1; i <= L; i += 2) dl ^= G.mul(C[i], G.pw(xi * (i - 1)));
        if (!dl) return -1;
        const uint8_t y = G.mul(G.pw(11 * e * (1 - 112)), G.div(om, dl));
        c[j] ^= y;
        found++;
    }
    if (found != L) return -1;
    // check
    for (int i = 0; i < kRsParity; i++) {
        const uint8_t r = G.pw(11 * (112 + i));
        uint8_t s = 0;
        for (size_t j = 0; j < n; j++) s = G.mul(s, r) ^ c[j];
        if (s) return -1;
    }
    return found;
}

void rsParityDual(const uint8_t* data, size_t n, uint8_t* par) {
    const Gf& G = gf();
    std::vector<uint8_t> d(n);
    for (size_t i = 0; i < n; i++) d[i] = G.tal1[data[i]];
    rsParity(d.data(), n, par);
    for (int i = 0; i < kRsParity; i++) par[i] = G.tal[par[i]];
}

int rsDecodeDual(uint8_t* block, size_t n) {
    const Gf& G = gf();
    std::vector<uint8_t> d(block, block + n);
    for (auto& v : d) v = G.tal1[v];
    const int r = rsDecode(d.data(), n);
    if (r < 0) return r;
    for (size_t i = 0; i < n; i++) block[i] = G.tal[d[i]];
    return r;
}

// ---------------------------------------------------------------- Golay (24,12)

namespace {

const uint16_t kGolayP[12] = {0x8ed, 0x1db, 0x3b5, 0x769, 0xed1, 0xda3, 0xb47, 0x68f, 0xd1d, 0xa3b, 0x477, 0xffe};

int parity(uint32_t v) { return dect2::bits::parity(v); }
int popcount(uint32_t v) { return dect2::bits::popcount(v); }

uint32_t golaySyndrome(uint32_t w) {
    const uint32_t data = w & 0xFFF, par = (w >> 12) & 0xFFF;
    uint32_t s = 0;
    for (int i = 0; i < 12; i++) s |= (uint32_t)(parity(kGolayP[i] & data) ^ ((par >> (11 - i)) & 1)) << (11 - i);
    return s;
}

struct GolayTable {
    std::vector<int32_t> err = std::vector<int32_t>(4096, -1);   // syndrome -> error pattern
    GolayTable() {
        err[0] = 0;
        for (int a = 0; a < 24; a++) {
            const uint32_t ea = 1u << a;
            if (err[golaySyndrome(ea)] < 0) err[golaySyndrome(ea)] = (int32_t)ea;
            for (int b = a + 1; b < 24; b++) {
                const uint32_t eb = ea | (1u << b);
                if (err[golaySyndrome(eb)] < 0) err[golaySyndrome(eb)] = (int32_t)eb;
                for (int c = b + 1; c < 24; c++) {
                    const uint32_t ec = eb | (1u << c);
                    if (err[golaySyndrome(ec)] < 0) err[golaySyndrome(ec)] = (int32_t)ec;
                }
            }
        }
    }
};

} // namespace

uint32_t golayEncode(uint32_t d) {
    d &= 0xFFF;
    uint32_t p = 0;
    for (int i = 0; i < 12; i++) p |= (uint32_t)parity(kGolayP[i] & d) << (11 - i);
    return (p << 12) | d;
}

int golayDecode(uint32_t w, int* errs) {
    static const GolayTable T;
    w &= 0xFFFFFF;
    const int32_t e = T.err[golaySyndrome(w)];
    if (e < 0) return -1;
    if (errs) *errs = popcount((uint32_t)e);
    return (int)((w ^ (uint32_t)e) & 0xFFF);
}

// ---------------------------------------------------------------- CCSDS randomiser

void ccsdsRandomize(uint8_t* d, size_t n) {
    uint8_t s = 0xFF;
    for (size_t i = 0; i < n; i++) {
        uint8_t o = 0;
        for (int k = 0; k < 8; k++) {
            const uint8_t bit = (uint8_t)(s >> 7);
            o = (uint8_t)((o << 1) | bit);
            const uint8_t fb = (uint8_t)(((s >> 7) ^ (s >> 4) ^ (s >> 2) ^ s) & 1);
            s = (uint8_t)((s << 1) | fb);
        }
        d[i] ^= o;
    }
}

// ---------------------------------------------------------------- frames

static void putAsm(std::vector<uint8_t>& v, uint32_t a) { for (int k = 3; k >= 0; k--) v.push_back((uint8_t)(a >> (8 * k))); }

static std::vector<uint8_t> rsBlock(const std::vector<uint8_t>& d) {
    std::vector<uint8_t> b(d.begin(), d.begin() + (ptrdiff_t)std::min(d.size(), (size_t)kRsMaxData));
    uint8_t p[kRsParity];
    rsParity(b.data(), b.size(), p);
    b.insert(b.end(), p, p + kRsParity);
    return b;
}

std::vector<uint8_t> ax100Mode5Encode(const std::vector<uint8_t>& csp) {
    std::vector<uint8_t> b = rsBlock(csp);
    ccsdsRandomize(b.data(), b.size());
    std::vector<uint8_t> v;
    putAsm(v, kAsmAx100);
    const uint32_t g = golayEncode((uint32_t)(b.size() & 0xFF) | kAx100Scrambler | kAx100Rs);
    v.push_back((uint8_t)(g >> 16)); v.push_back((uint8_t)(g >> 8)); v.push_back((uint8_t)g);
    v.insert(v.end(), b.begin(), b.end());
    return v;
}

std::vector<uint8_t> ax100Mode6Encode(const std::vector<uint8_t>& csp) { return rsBlock(csp); }

std::vector<uint8_t> ccsdsEncode(const std::vector<uint8_t>& data, int frameLen) {
    frameLen = std::max(kRsParity + 1, std::min(255, frameLen));
    std::vector<uint8_t> d(data.begin(), data.begin() + (ptrdiff_t)std::min(data.size(), (size_t)(frameLen - kRsParity)));
    d.resize((size_t)(frameLen - kRsParity), 0);
    uint8_t p[kRsParity];
    rsParityDual(d.data(), d.size(), p);
    d.insert(d.end(), p, p + kRsParity);
    ccsdsRandomize(d.data(), d.size());
    std::vector<uint8_t> v;
    putAsm(v, kAsmCcsds);
    v.insert(v.end(), d.begin(), d.end());
    return v;
}

bool ax100Mode5Decode(uint8_t* body, size_t n, int flags, std::vector<uint8_t>& csp, int& rsErr) {
    rsErr = -1;
    if (flags & kAx100Viterbi) return false;            // the convolutional code is not supported
    if (flags & kAx100Scrambler) ccsdsRandomize(body, n);
    if (flags & kAx100Rs) {
        if (n <= (size_t)kRsParity) return false;
        rsErr = rsDecode(body, n);
        if (rsErr < 0) return false;
        csp.assign(body, body + n - kRsParity);
    } else {
        rsErr = 0;
        csp.assign(body, body + n);
    }
    return csp.size() >= 4;
}

bool ax100Mode6Decode(const std::vector<uint8_t>& h, std::vector<uint8_t>& csp, int& rsErr) {
    rsErr = -1;
    if (h.size() <= (size_t)kRsParity + 4 || h.size() > 255) return false;
    std::vector<uint8_t> b = h;
    rsErr = rsDecode(b.data(), b.size());
    if (rsErr < 0) return false;
    csp.assign(b.begin(), b.end() - kRsParity);
    return true;
}

bool ccsdsDecode(uint8_t* block, size_t n, std::vector<uint8_t>& data, int& rsErr) {
    rsErr = -1;
    if (n <= (size_t)kRsParity || n > 255) return false;
    ccsdsRandomize(block, n);
    rsErr = rsDecodeDual(block, n);
    if (rsErr < 0) return false;
    data.assign(block, block + n - kRsParity);
    return true;
}

bool parseCsp(const std::vector<uint8_t>& p, CspHeader& h) {
    if (p.size() < 4) return false;
    const uint32_t v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    h.prio = (int)(v >> 30); h.src = (int)((v >> 25) & 31); h.dst = (int)((v >> 20) & 31);
    h.dport = (int)((v >> 14) & 63); h.sport = (int)((v >> 8) & 63); h.flags = (int)(v & 0xFF);
    return true;
}

std::vector<uint8_t> buildCsp(const CspHeader& h, const std::vector<uint8_t>& payload) {
    const uint32_t v = ((uint32_t)(h.prio & 3) << 30) | ((uint32_t)(h.src & 31) << 25) | ((uint32_t)(h.dst & 31) << 20) |
                       ((uint32_t)(h.dport & 63) << 14) | ((uint32_t)(h.sport & 63) << 8) | (uint32_t)(h.flags & 0xFF);
    std::vector<uint8_t> o = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    o.insert(o.end(), payload.begin(), payload.end());
    return o;
}

// ---------------------------------------------------------------- the satellite table

std::vector<SatDef> defaultSats() {
    // starting points only: the user edits the table (frequencies and framings change as satellites launch and decay)
    return {
        {"ISS (ARISS packet)", 25544, 145.825, Ax25Afsk1200, 1200, "RS0ISS"},
        {"NO-44 (PCSat)", 26931, 145.825, Ax25Afsk1200, 1200, ""},
    };
}

std::string satsToText(const std::vector<SatDef>& v) {
    std::string s;
    for (const SatDef& d : v) {
        std::string n = d.name;
        for (char& c : n) if (c == '|' || c == ';') c = ' ';
        char b[256];
        snprintf(b, sizeof b, "%s|%d|%.4f|%d|%d|%s;", n.c_str(), d.norad, d.mhz, d.framing, d.baud, d.call.c_str());
        s += b;
    }
    return s;
}

std::vector<SatDef> satsFromText(const std::string& s) {
    std::vector<SatDef> v;
    std::stringstream ss(s);
    for (std::string item; std::getline(ss, item, ';');) {
        std::stringstream is(item);
        std::string f[6];
        int k = 0;
        while (k < 6 && std::getline(is, f[k], '|')) k++;
        if (k < 5 || f[0].empty()) continue;
        SatDef d;
        d.name = f[0]; d.norad = atoi(f[1].c_str()); d.mhz = atof(f[2].c_str());
        d.framing = std::max(0, std::min(kNumFramings - 1, atoi(f[3].c_str())));
        d.baud = atoi(f[4].c_str());
        if (k >= 6) d.call = f[5];
        v.push_back(d);
    }
    return v;
}

std::string hexDump(const std::vector<uint8_t>& d) {
    std::string o;
    char b[8];
    for (size_t i = 0; i < d.size(); i += 16) {
        snprintf(b, sizeof b, "%04zx ", i);
        o += b;
        for (size_t j = 0; j < 16; j++) {
            if (i + j < d.size()) { snprintf(b, sizeof b, " %02x", d[i + j]); o += b; }
            else o += "   ";
        }
        o += "  |";
        for (size_t j = 0; j < 16 && i + j < d.size(); j++) { const uint8_t c = d[i + j]; o += (c >= 32 && c < 127) ? (char)c : '.'; }
        o += "|\n";
    }
    return o;
}

std::string asciiOf(const std::vector<uint8_t>& d) {
    std::string o;
    for (uint8_t c : d) o += (c >= 32 && c < 127) ? (char)c : '.';
    return o;
}

} // namespace cubesat
} // namespace dect2
