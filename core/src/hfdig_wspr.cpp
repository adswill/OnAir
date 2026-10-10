// WSPR message coding (see hfdig_ftx_int.h): the 50-bit message (types 1, 2 and 3), the K=32 r=1/2 convolutional code
// (polynomials 0xF2D05351, 0xE4613C47), the bit-reversal interleaver and a Fano sequential decoder. Re-implemented from the
// descriptions of WSJT-X's wsprsim_utils.c / wsprd_utils.c / fano.c and JTEncode, which agree on every table used here.
#include "hfdig_ftx_int.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

namespace dect2 {
namespace ftx {

// ---------------------------------------------------------------- lookup3 (Bob Jenkins, public domain), hashlittle()

static inline uint32_t rot(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

uint32_t nhash(const void* key, size_t length, uint32_t initval) {
    uint32_t a, b, c;
    a = b = c = 0xdeadbeef + (uint32_t)length + initval;
    const uint8_t* k = (const uint8_t*)key;
    while (length > 12) {
        a += k[0] + ((uint32_t)k[1] << 8) + ((uint32_t)k[2] << 16) + ((uint32_t)k[3] << 24);
        b += k[4] + ((uint32_t)k[5] << 8) + ((uint32_t)k[6] << 16) + ((uint32_t)k[7] << 24);
        c += k[8] + ((uint32_t)k[9] << 8) + ((uint32_t)k[10] << 16) + ((uint32_t)k[11] << 24);
        a -= c; a ^= rot(c, 4);  c += b;
        b -= a; b ^= rot(a, 6);  a += c;
        c -= b; c ^= rot(b, 8);  b += a;
        a -= c; a ^= rot(c, 16); c += b;
        b -= a; b ^= rot(a, 19); a += c;
        c -= b; c ^= rot(b, 4);  b += a;
        length -= 12; k += 12;
    }
    switch (length) {
    case 12: c += (uint32_t)k[11] << 24; [[fallthrough]];
    case 11: c += (uint32_t)k[10] << 16; [[fallthrough]];
    case 10: c += (uint32_t)k[9] << 8; [[fallthrough]];
    case 9: c += k[8]; [[fallthrough]];
    case 8: b += (uint32_t)k[7] << 24; [[fallthrough]];
    case 7: b += (uint32_t)k[6] << 16; [[fallthrough]];
    case 6: b += (uint32_t)k[5] << 8; [[fallthrough]];
    case 5: b += k[4]; [[fallthrough]];
    case 4: a += (uint32_t)k[3] << 24; [[fallthrough]];
    case 3: a += (uint32_t)k[2] << 16; [[fallthrough]];
    case 2: a += (uint32_t)k[1] << 8; [[fallthrough]];
    case 1: a += k[0]; break;
    case 0: return c;
    }
    c ^= b; c -= rot(b, 14);
    a ^= c; a -= rot(c, 11);
    b ^= a; b -= rot(a, 25);
    c ^= b; c -= rot(b, 16);
    a ^= c; a -= rot(c, 4);
    b ^= a; b -= rot(a, 14);
    c ^= b; c -= rot(b, 24);
    return c;
}

namespace {

uint32_t callHash15(const std::string& call) { return nhash(call.data(), call.size(), 146) & 32767; }

int code(char c) {   // WSPR's character code: digits 0..9, letters 10..35, space 36
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    if (c == ' ') return 36;
    return -1;
}
char uncode(int v) { return v < 10 ? (char)('0' + v) : v < 36 ? (char)('A' + v - 10) : ' '; }

// a callsign of up to 6 characters -> 28 bits (-1 if it does not fit)
int64_t packCall(const std::string& c) {
    if (c.empty() || c.size() > 6) return -1;
    std::string s;
    if (c.size() > 2 && isdigit((unsigned char)c[2])) s = c;
    else if (c.size() > 1 && isdigit((unsigned char)c[1]) && c.size() <= 5) s = " " + c;
    else return -1;
    while (s.size() < 6) s += ' ';
    const int c0 = code(s[0]), c1 = code(s[1]), c2 = code(s[2]);
    if (c0 < 0 || c1 < 0 || c1 == 36 || c2 < 0 || c2 > 9) return -1;
    int64_t n = c0;
    n = n * 36 + c1;
    n = n * 10 + c2;
    for (int k = 3; k < 6; k++) {
        const int v = code(s[k]);
        if (v < 10) return -1;   // letters or space only
        n = n * 27 + (v - 10);
    }
    return n;
}

bool unpackCall(uint32_t n, std::string& out) {
    if (n >= 262177560u) return false;
    char c[7] = {};
    c[5] = uncode((int)(n % 27) + 10); n /= 27;
    c[4] = uncode((int)(n % 27) + 10); n /= 27;
    c[3] = uncode((int)(n % 27) + 10); n /= 27;
    c[2] = uncode((int)(n % 10)); n /= 10;
    c[1] = uncode((int)(n % 36)); n /= 36;
    c[0] = uncode((int)n);
    std::string s = c;
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();
    out = s;
    return !s.empty();
}

bool validPower(int p) { const int u = p % 10; return p >= 0 && p <= 60 && (u == 0 || u == 3 || u == 7); }

void put50(uint32_t n, uint32_t m, uint8_t* d) {
    d[0] = (uint8_t)(n >> 20); d[1] = (uint8_t)(n >> 12); d[2] = (uint8_t)(n >> 4);
    d[3] = (uint8_t)(((n & 15) << 4) | ((m >> 18) & 15));
    d[4] = (uint8_t)(m >> 10); d[5] = (uint8_t)(m >> 2); d[6] = (uint8_t)((m & 3) << 6);
}

} // namespace

bool wsprPack(const std::string& msg, uint8_t* d) {
    std::string up;
    for (char c : msg) up += (char)toupper((unsigned char)c);
    std::istringstream is(up);
    std::vector<std::string> t;
    std::string w;
    while (is >> w) t.push_back(w);
    if (t.empty()) return false;
    const int power = std::atoi(t.back().c_str());
    if (!validPower(power)) return false;
    for (char c : t.back()) if (!isdigit((unsigned char)c)) return false;
    if (t.size() == 3 && t[0].size() > 2 && t[0].front() == '<' && t[0].back() == '>') {   // type 3: <call> grid6 power
        const std::string call = t[0].substr(1, t[0].size() - 2), g = t[1];
        if (g.size() != 6) return false;
        const int64_t n = packCall(g.substr(1) + g.substr(0, 1));
        if (n < 0) return false;
        const int ntype = -(power + 1);
        const uint32_t m = (uint32_t)(128 * (int)callHash15(call) + ntype + 64);
        put50((uint32_t)n, m, d);
        return true;
    }
    if (t.size() == 3) {   // type 1: call grid4 power
        const std::string& g = t[1];
        if (g.size() != 4 || g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R' || !isdigit((unsigned char)g[2]) || !isdigit((unsigned char)g[3]))
            return false;
        const int64_t n = packCall(t[0]);
        if (n < 0) return false;
        uint32_t m = (uint32_t)((179 - 10 * (g[0] - 'A') - (g[2] - '0')) * 180 + 10 * (g[1] - 'A') + (g[3] - '0'));
        m = m * 128 + (uint32_t)power + 64;
        put50((uint32_t)n, m, d);
        return true;
    }
    if (t.size() == 2) {   // type 2: prefix/call or call/suffix, power
        const std::string& c = t[0];
        const size_t sl = c.find('/');
        if (sl == std::string::npos || c.find('/', sl + 1) != std::string::npos) return false;
        std::string base;
        int ng;
        if (sl > 0 && sl <= 3 && c.size() - sl - 1 > 2) {   // prefix
            const std::string pfx = c.substr(0, sl);
            base = c.substr(sl + 1);
            int m = pfx.size() == 1 ? 37 * 36 + 36 : pfx.size() == 2 ? 36 : 0;
            for (char ch : pfx) { const int v = code(ch); if (v < 0 || v == 36) return false; m = 37 * m + v; }
            ng = m;
        } else {   // suffix: one character, or two digits
            base = c.substr(0, sl);
            const std::string sfx = c.substr(sl + 1);
            if (sfx.size() == 1 && code(sfx[0]) >= 0 && code(sfx[0]) < 36) ng = 60000 + code(sfx[0]);
            else if (sfx.size() == 2 && isdigit((unsigned char)sfx[0]) && isdigit((unsigned char)sfx[1])) ng = 60000 + 26 + 10 * (sfx[0] - '0') + (sfx[1] - '0');
            else return false;
        }
        const int64_t n = packCall(base);
        if (n < 0) return false;
        int nadd = 1;
        if (ng >= 32768) { ng -= 32768; nadd = 2; }
        const int ntype = power + nadd;
        const uint32_t m = (uint32_t)(128 * ng + ntype + 64);
        put50((uint32_t)n, m, d);
        return true;
    }
    return false;
}

std::string wsprUnpack(const uint8_t* d, std::map<uint32_t, std::string>& table, int* dbm) {
    const uint32_t n1 = ((uint32_t)d[0] << 20) | ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | ((d[3] >> 4) & 15);
    const uint32_t n2 = ((uint32_t)(d[3] & 15) << 18) | ((uint32_t)d[4] << 10) | ((uint32_t)d[5] << 2) | ((d[6] >> 6) & 3);
    std::string call;
    if (!unpackCall(n1, call)) return "";
    const int ntype = (int)(n2 & 127) - 64;
    const uint32_t ng = n2 >> 7;
    char b[64];
    if (ntype >= 0 && ntype <= 62) {
        const int nu = ntype % 10;
        if (nu == 0 || nu == 3 || nu == 7) {   // type 1
            if (ng >= 32400) return "";
            const int a = (int)(179 - ng / 180) / 10, c = (int)(179 - ng / 180) % 10, bb = (int)(ng % 180) / 10, dd = (int)ng % 10;
            if (a < 0 || a > 17 || bb > 17) return "";
            char g[5] = {(char)('A' + a), (char)('A' + bb), (char)('0' + c), (char)('0' + dd), 0};
            if (call.size() > 6 || call.find(' ') != std::string::npos) return "";
            table[callHash15(call)] = call;
            if (dbm) *dbm = ntype;
            snprintf(b, sizeof b, "%s %s %d", call.c_str(), g, ntype);
            return b;
        }
        int nadd = nu;
        if (nu > 3) nadd = nu - 3;
        if (nu > 7) nadd = nu - 7;
        const uint32_t n3 = ng + 32768u * (uint32_t)(nadd - 1);
        std::string full;
        if (n3 < 60000) {
            uint32_t v = n3;
            char p[4] = {};
            p[2] = uncode((int)(v % 37)); v /= 37;
            p[1] = uncode((int)(v % 37)); v /= 37;
            if (v >= 37) return "";
            p[0] = uncode((int)v);
            std::string pf = p;
            const size_t sp = pf.rfind(' ');
            if (sp != std::string::npos) pf = pf.substr(sp + 1);
            if (pf.empty()) return "";
            full = pf + "/" + call;
        } else {
            const uint32_t nc = n3 - 60000;
            if (nc <= 9) full = call + "/" + (char)('0' + nc);
            else if (nc <= 35) full = call + "/" + (char)('A' + nc - 10);
            else if (nc <= 125) full = call + "/" + (char)('0' + (nc - 26) / 10) + (char)('0' + (nc - 26) % 10);
            else return "";
        }
        const int p = ntype - nadd;
        if (!validPower(p)) return "";
        table[callHash15(full)] = full;
        if (dbm) *dbm = p;
        snprintf(b, sizeof b, "%s %d", full.c_str(), p);
        return b;
    }
    if (ntype < 0 && ntype != -64) {   // type 3: the "call" is the six-character grid, rotated
        const int p = -(ntype + 1);
        if (call.size() != 6 || !validPower(p)) return "";
        const std::string g = call.substr(5) + call.substr(0, 5);
        if (g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R' || !isdigit((unsigned char)g[2]) || !isdigit((unsigned char)g[3]) ||
            g[4] < 'A' || g[4] > 'X' || g[5] < 'A' || g[5] > 'X')
            return "";
        const uint32_t ih = (uint32_t)((int)n2 - ntype - 64) / 128;
        auto it = table.find(ih);
        const std::string c = it == table.end() ? "<...>" : "<" + it->second + ">";
        if (dbm) *dbm = p;
        snprintf(b, sizeof b, "%s %s %d", c.c_str(), g.c_str(), p);
        return b;
    }
    return "";
}

// ---------------------------------------------------------------- the convolutional code and the interleaver

static constexpr uint32_t kPoly1 = 0xF2D05351u, kPoly2 = 0xE4613C47u;
static inline int parity(uint32_t x) {
    x ^= x >> 16; x ^= x >> 8; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;
    return (int)(x & 1);
}

void wsprConvEncode(const uint8_t* d, uint8_t* out) {
    uint32_t reg = 0;
    for (int i = 0; i < 81; i++) {
        const int bit = i < 50 ? (d[i / 8] >> (7 - i % 8)) & 1 : 0;
        reg = (reg << 1) | (uint32_t)bit;
        out[2 * i] = (uint8_t)parity(reg & kPoly1);
        out[2 * i + 1] = (uint8_t)parity(reg & kPoly2);
    }
}

static int rev8(int j) { int r = 0; for (int b = 0; b < 8; b++) if (j & (1 << b)) r |= 1 << (7 - b); return r; }

void wsprInterleave(const uint8_t* in, uint8_t* out) {
    int p = 0;
    for (int j = 0; j < 256 && p < 162; j++) { const int r = rev8(j); if (r < 162) out[r] = in[p++]; }
}
void wsprDeinterleave(const float* in, float* out) {
    int p = 0;
    for (int j = 0; j < 256 && p < 162; j++) { const int r = rev8(j); if (r < 162) out[p++] = in[r]; }
}

bool wsprSymbols(const std::string& msg, std::vector<int>& sym) {
    uint8_t d[7], coded[162], ch[162];
    if (!wsprPack(msg, d)) return false;
    wsprConvEncode(d, coded);
    wsprInterleave(coded, ch);
    sym.resize(162);
    for (int i = 0; i < 162; i++) sym[(size_t)i] = kWsprSync[i] + 2 * ch[i];
    return true;
}

// ---------------------------------------------------------------- Fano sequential decoder (after P. Karn's fano.c)

bool wsprFano(const float* llr, uint8_t* d, long maxCycles) {
    constexpr int kBits = 81, kData = 50;
    // the metric of a coded bit: log2(2 P(bit | y)) - rate, in units of 1/8 bit
    auto met = [](float l, int bit) {
        const float x = bit ? l : -l;
        const float p = 1.f / (1.f + std::exp(-std::max(-20.f, std::min(20.f, x))));
        return (int)std::lround(8.0 * (std::log2(2.0 * std::max(1e-6f, p)) - 0.5));
    };
    int m[162][2];
    for (int i = 0; i < 162; i++) { m[i][0] = met(llr[i], 0); m[i][1] = met(llr[i], 1); }
    struct Node { uint32_t enc; int gamma; int tm[2]; int i; };
    std::vector<Node> nodes(kBits + 1);
    auto branch = [&](int k, uint32_t state, int bit) {   // state: the register with this bit shifted in
        (void)bit;
        return m[2 * k][parity(state & kPoly1)] + m[2 * k + 1][parity(state & kPoly2)];
    };
    auto fill = [&](int k) {
        Node& n = nodes[(size_t)k];
        const uint32_t s0 = n.enc;   // LSB 0
        const int m0 = branch(k, s0, 0);
        if (k >= kData) { n.tm[0] = m0; n.tm[1] = 0; return; }
        const int m1 = branch(k, s0 | 1, 1);
        if (m0 >= m1) { n.tm[0] = m0; n.tm[1] = m1; }
        else { n.tm[0] = m1; n.tm[1] = m0; n.enc |= 1; }
    };
    const int delta = 16;
    int t = 0;
    nodes[0].enc = 0; nodes[0].gamma = 0; nodes[0].i = 0;
    fill(0);
    int np = 0;
    bool done = false;
    for (long cyc = 0; cyc < maxCycles; cyc++) {
        Node& n = nodes[(size_t)np];
        const int ng = n.gamma + n.tm[n.i];
        if (ng >= t) {
            if (n.gamma < t + delta) while (ng >= t + delta) t += delta;   // first visit: tighten the threshold
            Node& nx = nodes[(size_t)np + 1];
            nx.gamma = ng;
            nx.enc = n.enc << 1;
            if (++np == kBits) { done = true; break; }
            fill(np);
            nodes[(size_t)np].i = 0;
            continue;
        }
        for (;;) {   // look back
            if (np == 0 || nodes[(size_t)np - 1].gamma < t) {
                t -= delta;
                if (nodes[(size_t)np].i != 0) { nodes[(size_t)np].i = 0; nodes[(size_t)np].enc ^= 1; }
                break;
            }
            --np;
            if (np < kData && nodes[(size_t)np].i != 1) { nodes[(size_t)np].i++; nodes[(size_t)np].enc ^= 1; break; }
        }
    }
    if (!done) return false;
    std::memset(d, 0, 7);
    for (int k = 0; k < kData; k++) if (nodes[(size_t)k].enc & 1) d[k / 8] |= (uint8_t)(0x80 >> (k % 8));
    return true;
}

} // namespace ftx
} // namespace dect2
