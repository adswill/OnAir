// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ARDOP frame layer (see ardop_proto.h).
#include "dect2/ardop/ardop_proto.h"
#include <algorithm>
#include <cstring>

namespace dect2 {
namespace ardop {

namespace {

struct DataRow { int code; const char* mod; int carriers, baud, bw, data, rs; const char* name; };
// the even code of each pair (the odd one differs in the lowest bit and has the same parameters)
const DataRow kData[] = {
    {0x40, "4PSK", 1, 100, 200, 64, 32, "4PSK.200.100"},
    {0x42, "4PSK", 1, 100, 200, 16, 8, "4PSK.200.100S"},
    {0x44, "8PSK", 1, 100, 200, 108, 36, "8PSK.200.100"},
    {0x46, "16QAM", 1, 100, 200, 128, 64, "16QAM.200.100"},
    {0x48, "4FSK", 1, 50, 200, 16, 4, "4FSK.200.50S"},
    {0x4A, "4FSK", 1, 100, 500, 64, 16, "4FSK.500.100"},
    {0x4C, "4FSK", 1, 100, 500, 32, 8, "4FSK.500.100S"},
    {0x50, "4PSK", 2, 100, 500, 64, 32, "4PSK.500.100"},
    {0x52, "8PSK", 2, 100, 500, 108, 36, "8PSK.500.100"},
    {0x54, "16QAM", 2, 100, 500, 128, 64, "16QAM.500.100"},
    {0x60, "4PSK", 4, 100, 1000, 64, 32, "4PSK.1000.100"},
    {0x62, "8PSK", 4, 100, 1000, 108, 36, "8PSK.1000.100"},
    {0x64, "16QAM", 4, 100, 1000, 128, 64, "16QAM.1000.100"},
    {0x70, "4PSK", 8, 100, 2000, 64, 32, "4PSK.2000.100"},
    {0x72, "8PSK", 8, 100, 2000, 108, 36, "8PSK.2000.100"},
    {0x74, "16QAM", 8, 100, 2000, 128, 64, "16QAM.2000.100"},
    {0x7A, "4FSK", 1, 600, 2000, 600, 150, "4FSK.2000.600"},
    {0x7C, "4FSK", 1, 600, 2000, 200, 50, "4FSK.2000.600S"},
};

// GF(256), x^8 + x^4 + x^3 + x^2 + 1, alpha = 2
struct Gf {
    uint8_t ex[512], lg[256];
    Gf() {
        int x = 1;
        for (int i = 0; i < 255; i++) { ex[i] = (uint8_t)x; lg[x] = (uint8_t)i; x <<= 1; if (x & 0x100) x ^= 0x11D; }
        for (int i = 255; i < 512; i++) ex[i] = ex[i - 255];
        lg[0] = 0;
    }
    uint8_t mul(int a, int b) const { return (a && b) ? ex[lg[a] + lg[b]] : 0; }
    uint8_t inv(int a) const { return ex[255 - lg[a]]; }
};
const Gf& gf() { static const Gf g; return g; }

// generator polynomial: product of (x - alpha^i), i = 1 .. n; g[n] = 1
std::vector<uint8_t> genPoly(int n) {
    const Gf& f = gf();
    std::vector<uint8_t> g((size_t)n + 1, 0);
    g[0] = 1;
    for (int i = 1; i <= n; i++) {
        const uint8_t a = f.ex[i];
        for (int j = i; j >= 0; j--) g[(size_t)j] = (uint8_t)((j ? g[(size_t)j - 1] : 0) ^ f.mul(g[(size_t)j], a));
    }
    return g;
}

// the power of x of byte i of block (data first, then n parity bytes): the data sit at the high powers, zero padded below them, the parity at x^0 ..
inline int powerOf(int i, int len, int n) {
    const int d = len - n;
    return i < d ? n + (255 - n - d) + i : i - d;
}

} // namespace

bool frameDef(int code, FrameDef& o) {
    o = FrameDef();
    o.code = code;
    char b[40];
    if (code >= 0x00 && code <= 0x1F) { o.kind = KNak; o.name = "DataNAK"; return true; }
    if (code >= 0xE0 && code <= 0xFF) { o.kind = KAck; o.name = "DataACK"; return true; }
    switch (code) {
    case 0x23: o.kind = KBreak; o.name = "BREAK"; return true;
    case 0x24: o.kind = KIdle; o.name = "IDLE"; return true;
    case 0x29: o.kind = KDisc; o.name = "DISC"; return true;
    case 0x2C: o.kind = KEnd; o.name = "END"; return true;
    case 0x2D: o.kind = KConRejBusy; o.name = "ConRejBusy"; return true;
    case 0x2E: o.kind = KConRejBw; o.name = "ConRejBW"; return true;
    case 0x30: o.kind = KId; o.name = "IDFrame"; return true;
    case 0x3D: o.kind = KPingAck; o.name = "PingAck"; return true;
    case 0x3E: o.kind = KPing; o.name = "Ping"; return true;
    default: break;
    }
    if (code >= 0x31 && code <= 0x38) {
        static const int bw[4] = {200, 500, 1000, 2000};
        o.kind = KConReq; o.bandwidth = bw[(code - 0x31) & 3];
        snprintf(b, sizeof b, "ConReq%d%c", o.bandwidth, code >= 0x35 ? 'F' : 'M');
        o.name = b;
        return true;
    }
    if (code >= 0x39 && code <= 0x3C) {
        static const int bw[4] = {200, 500, 1000, 2000};
        o.kind = KConAck; o.bandwidth = bw[code - 0x39];
        snprintf(b, sizeof b, "ConAck%d", o.bandwidth);
        o.name = b;
        return true;
    }
    for (const DataRow& r : kData) {
        if ((code & ~1) != r.code) continue;
        o.kind = KData; o.mod = r.mod; o.carriers = r.carriers; o.baud = r.baud; o.bandwidth = r.bw;
        o.dataBytes = r.data; o.rsBytes = r.rs; o.odd = (code & 1) != 0;
        o.name = std::string(r.name) + (o.odd ? ".O" : ".E");
        o.fsk = !strcmp(r.mod, "4FSK") && r.baud <= 100;
        return true;
    }
    return false;
}

int ackQuality(int code) { return 38 + 2 * (code & 0x1F); }

uint8_t typeParity(uint8_t t) {
    uint8_t s = 1;
    for (int k = 0; k < 4; k++) s ^= (uint8_t)((t >> (6 - 2 * k)) & 3);
    return s & 3;
}

uint16_t crc16(const uint8_t* d, size_t n) {
    int reg = 0xFFFF;
    for (size_t j = 0; j < n; j++)
        for (int m = 0x80; m; m >>= 1) {
            const int bit = (d[j] & m) ? 1 : 0;
            const bool top = (reg & 0x8000) != 0;
            reg = ((reg << 1) | bit) & 0xFFFF;
            if (top) reg ^= 0x8810;
        }
    return (uint16_t)reg;
}

void rsAppend(std::vector<uint8_t>& block, int n) {
    const Gf& f = gf();
    const int d = (int)block.size(), kk = 255 - n;
    if (d > kk || n < 1) return;
    static std::vector<uint8_t> cache[129];
    if (n <= 128 && cache[n].empty()) cache[n] = genPoly(n);
    const std::vector<uint8_t> g = n <= 128 ? cache[n] : genPoly(n);
    std::vector<uint8_t> u((size_t)kk, 0), rem((size_t)n, 0);
    for (int i = 0; i < d; i++) u[(size_t)(kk - d + i)] = block[(size_t)i];
    for (int p = kk - 1; p >= 0; p--) {
        const uint8_t fb = u[(size_t)p] ^ rem[(size_t)n - 1];
        for (int j = n - 1; j > 0; j--) rem[(size_t)j] = (uint8_t)(rem[(size_t)j - 1] ^ f.mul(fb, g[(size_t)j]));
        rem[0] = f.mul(fb, g[0]);
    }
    for (int j = 0; j < n; j++) block.push_back(rem[(size_t)j]);
}

int rsCorrect(uint8_t* r, int len, int n) {
    const Gf& f = gf();
    if (len <= n || len > 255 || n < 1 || n > 128) return -1;
    std::vector<int> S((size_t)n + 1, 0);
    bool any = false;
    for (int j = 1; j <= n; j++) {
        int s = 0;
        for (int i = 0; i < len; i++) if (r[i]) s ^= f.ex[(f.lg[r[i]] + powerOf(i, len, n) * j) % 255];
        S[(size_t)j] = s;
        any = any || s;
    }
    if (!any) return 0;
    // Berlekamp-Massey
    std::vector<int> C(n + 2, 0), B(n + 2, 0), T;
    C[0] = B[0] = 1;
    int L = 0, m = 1, bb = 1;
    for (int i = 0; i < n; i++) {
        int dlt = S[(size_t)i + 1];
        for (int j = 1; j <= L; j++) dlt ^= f.mul(C[(size_t)j], S[(size_t)(i + 1 - j)]);
        if (!dlt) { m++; continue; }
        T = C;
        const int coef = f.mul(dlt, f.inv(bb));
        for (int j = 0; j + m <= n; j++) C[(size_t)(j + m)] ^= f.mul(coef, B[(size_t)j]);
        if (2 * L <= i) { L = i + 1 - L; B = T; bb = dlt; m = 1; } else m++;
    }
    if (L > n / 2) return -1;
    // Chien search over the positions that exist
    std::vector<int> pos, xs;
    for (int i = 0; i < len; i++) {
        const int p = powerOf(i, len, n);
        // root test: Lambda(alpha^-p) = 0
        int v = 0;
        for (int j = 0; j <= L; j++) if (C[(size_t)j]) v ^= f.ex[(f.lg[C[(size_t)j]] + ((255 - p) % 255) * j) % 255];
        if (!v) { pos.push_back(i); xs.push_back(p); }
    }
    if ((int)pos.size() != L) return -1;
    // Omega = S(x) * Lambda(x) mod x^n, S(x) = sum S[j+1] x^j
    std::vector<int> om((size_t)n, 0);
    for (int a = 0; a < n; a++)
        for (int j = 0; j <= L && j <= a; j++) om[(size_t)a] ^= f.mul(C[(size_t)j], S[(size_t)(a - j + 1)]);
    std::vector<uint8_t> fix((size_t)len);
    memcpy(fix.data(), r, (size_t)len);
    for (size_t k = 0; k < pos.size(); k++) {
        const int xinv = (255 - xs[k]) % 255;
        int num = 0, den = 0;
        for (int a = 0; a < n; a++) if (om[(size_t)a]) num ^= f.ex[(f.lg[om[(size_t)a]] + xinv * a) % 255];
        for (int j = 1; j <= L; j += 2) if (C[(size_t)j]) den ^= f.ex[(f.lg[C[(size_t)j]] + xinv * (j - 1)) % 255];   // the odd terms of the derivative
        if (!den) return -1;
        fix[(size_t)pos[k]] ^= f.mul(num, f.inv(den));
    }
    for (int j = 1; j <= n; j++) {   // verify
        int s = 0;
        for (int i = 0; i < len; i++) if (fix[(size_t)i]) s ^= f.ex[(f.lg[fix[(size_t)i]] + powerOf(i, len, n) * j) % 255];
        if (s) return -1;
    }
    memcpy(r, fix.data(), (size_t)len);
    return (int)pos.size();
}

std::array<uint8_t, 6> packed6(const std::string& s) {
    char w[9];
    memset(w, ' ', 8); w[8] = 0;
    for (size_t i = 0; i < s.size() && i < 8; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
        w[i] = (c >= 32 && c <= 95) ? (char)c : ' ';
    }
    std::array<uint8_t, 6> o{};
    for (int h = 0; h < 2; h++) {
        uint32_t p = 0;
        for (int i = 0; i < 4; i++) p = (p << 6) | (uint32_t)((unsigned char)w[h * 4 + i] - 32);
        o[(size_t)h * 3] = (uint8_t)(p >> 16); o[(size_t)h * 3 + 1] = (uint8_t)(p >> 8); o[(size_t)h * 3 + 2] = (uint8_t)p;
    }
    return o;
}

std::string unpacked6(const uint8_t* b) {
    std::string s(8, ' ');
    for (int h = 0; h < 2; h++) {
        const uint32_t p = ((uint32_t)b[h * 3] << 16) | ((uint32_t)b[h * 3 + 1] << 8) | b[h * 3 + 2];
        for (int i = 0; i < 4; i++) s[(size_t)(h * 4 + i)] = (char)(32 + ((p >> (6 * (3 - i))) & 0x3F));
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

static std::vector<uint8_t> twoPacked(const std::string& a, const std::string& b) {
    std::vector<uint8_t> v;
    const auto x = packed6(a), y = packed6(b);
    v.insert(v.end(), x.begin(), x.end());
    v.insert(v.end(), y.begin(), y.end());
    rsAppend(v, 4);
    return v;
}
std::vector<uint8_t> makeIdFrame(const std::string& call, const std::string& loc) { return twoPacked(call, loc); }
std::vector<uint8_t> makeCallFrame(int, const std::string& from, const std::string& to) { return twoPacked(from, to); }
std::vector<uint8_t> makeConAck(int, int leaderMs, uint8_t) {
    const uint8_t t = (leaderMs < 0 || leaderMs > 2550) ? 0 : (uint8_t)std::min(255, leaderMs / 10);
    return {t, t, t};
}
std::vector<uint8_t> makePingAck(int snrDb, int quality) {
    uint8_t b = snrDb >= 21 ? 0xF8 : (uint8_t)(((snrDb + 10) & 0x1F) << 3);
    b = (uint8_t)(b + (std::max(0, (quality - 30) / 10) & 7));
    return {b, b, b};
}
std::vector<uint8_t> makeControl(int, uint8_t) { return {}; }

bool makeDataBlock(int code, const std::vector<uint8_t>& payload, std::vector<uint8_t>& block) {
    FrameDef d;
    if (!frameDef(code, d) || d.kind != KData || !d.fsk || (int)payload.size() > d.dataBytes) return false;
    block.assign((size_t)d.dataBytes + 1, 0);
    block[0] = (uint8_t)payload.size();
    std::copy(payload.begin(), payload.end(), block.begin() + 1);
    const uint16_t c = crc16(block.data(), block.size());
    block.push_back((uint8_t)(c >> 8));
    block.push_back((uint8_t)((c & 0xFF) ^ (code & 0xFF)));
    rsAppend(block, d.rsBytes);
    return true;
}

std::vector<uint8_t> makeFrameBytes(int code, uint8_t sid, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> v{(uint8_t)code, (uint8_t)(code ^ sid)};
    v.insert(v.end(), body.begin(), body.end());
    return v;
}

int bodyLength(int code) {
    FrameDef d;
    if (!frameDef(code, d)) return -1;
    switch (d.kind) {
    case KId: case KConReq: case KPing: return 16;
    case KConAck: case KPingAck: return 3;
    case KData: return d.fsk ? 1 + d.dataBytes + 2 + d.rsBytes : -1;
    default: return 0;
    }
}

bool parseBody(int code, const uint8_t* body, size_t n, Parsed& o) {
    FrameDef d;
    o.code = code;
    if (!frameDef(code, d)) return false;
    const int need = bodyLength(code);
    if (need < 0 || (int)n < need) return need == 0;
    switch (d.kind) {
    case KId: case KConReq: case KPing: {
        uint8_t b[16];
        memcpy(b, body, 16);
        const int fx = rsCorrect(b, 16, 4);
        if (fx < 0) return false;
        o.rsFixed = fx;
        const std::string a = unpacked6(b), c = unpacked6(b + 6);
        o.from = a;
        if (d.kind == KId) o.locator = c; else o.to = c;
        o.bodyOk = true;
        return true;
    }
    case KConAck: case KPingAck: {
        int v[256] = {0}, best = body[0], bn = 0;
        for (int i = 0; i < 3; i++) if (++v[body[i]] > bn) { bn = v[body[i]]; best = body[i]; }
        if (bn < 2) return false;   // no two copies agree
        if (d.kind == KConAck) o.leaderMs = best * 10;
        else { o.snrDb = ((best & 0xF8) >> 3) - 10; o.quality = (best & 7) * 10 + 30; }
        o.bodyOk = true;
        return true;
    }
    case KData: {
        std::vector<uint8_t> b(body, body + need);
        const int fx = rsCorrect(b.data(), need, d.rsBytes);
        if (fx < 0) return false;
        const size_t dl = (size_t)d.dataBytes + 1;
        const uint16_t c = crc16(b.data(), dl);
        if (b[dl] != (c >> 8) || b[dl + 1] != ((c & 0xFF) ^ (code & 0xFF))) return false;
        o.rsFixed = fx;
        const size_t cnt = std::min<size_t>(b[0], (size_t)d.dataBytes);
        o.payload.assign(b.begin() + 1, b.begin() + 1 + (long)cnt);
        o.bodyOk = true;
        return true;
    }
    default: o.bodyOk = true; return true;
    }
}

} // namespace ardop
} // namespace dect2
