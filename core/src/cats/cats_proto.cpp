// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors
// CATS framing and whiskers (cats.h). The LDPC chunk choice and the circulant encoder follow libCATS (Cam K. VE3KCN, BSD-3-Clause).
#include "dect2/cats/cats.h"
#include <cmath>
#include <cstring>

namespace dect2 {
namespace cats {
namespace tables {
extern const uint64_t tc128_matrix[], tc256_matrix[], tc512_matrix[], tm2048_matrix[], tm8192_matrix[];
}

namespace {
struct Code { int n, k, circ; const uint64_t* m; };   // code length and data length in bits, circulant size, matrix

const Code& pickCode(size_t len) {
    static const Code c128{128, 64, 128 / 8, tables::tc128_matrix};
    static const Code c256{256, 128, 256 / 8, tables::tc256_matrix};
    static const Code c512{512, 256, 512 / 8, tables::tc512_matrix};
    static const Code m2048{2048, 1024, 512 / 4, tables::tm2048_matrix};
    static const Code m8192{8192, 4096, 2048 / 4, tables::tm8192_matrix};
    if (len >= 512) return m8192;
    if (len >= 128) return m2048;
    if (len >= 32) return c512;
    if (len >= 16) return c256;
    return c128;
}

inline bool getBit(const uint8_t* d, size_t i) { return (d[i / 8] >> (7 - i % 8)) & 1; }

void encodeChunk(const uint8_t* data, const Code& c, uint8_t* parity) {
    const int dataBits = c.k, parBits = c.n - c.k, circ = c.circ, rowLen = parBits / 64;
    std::memset(parity, 0, parBits / 8);
    for (int off = 0; off < circ; off++) {
        for (int crow = 0; crow < dataBits / circ; crow++) {
            if (getBit(data, (size_t)crow * circ + off)) {
                for (int idx = 0; idx < rowLen; idx++) {
                    const uint64_t w = c.m[crow * rowLen + idx];
                    for (int j = 0; j < 8; j++) parity[idx * 8 + j] ^= (uint8_t)(w >> ((7 - j) * 8));
                }
            }
        }
        for (int block = 0; block < parBits / circ; block++) {
            uint8_t* pb = &parity[block * circ / 8];
            uint8_t carry = pb[0] >> 7;
            for (int x = circ / 8 - 1; x >= 0; x--) {
                const uint8_t cc = pb[x] >> 7;
                pb[x] = (uint8_t)((pb[x] << 1) | carry);
                carry = cc;
            }
        }
    }
}

void whiten(std::vector<uint8_t>& d, size_t n) {
    uint16_t s = 0xE9CF;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = 0;
        for (int k = 7; k >= 0; k--) {
            b |= (uint8_t)((s & 1) << k);
            const bool lsb = s & 1;
            s >>= 1;
            if (lsb) s ^= 0xB400;
        }
        d[i] ^= b;
    }
}

// 32-bit block interleaver: out bit k = in bit (i + 32 j), i outer
void interleave(std::vector<uint8_t>& d, bool inverse) {
    const size_t nb = d.size() * 8;
    std::vector<uint8_t> o(d.size(), 0);
    size_t k = 0;
    for (size_t i = 0; i < 32; i++)
        for (size_t j = i; j < nb; j += 32, k++) {
            const size_t src = inverse ? k : j, dst = inverse ? j : k;
            if (getBit(d.data(), src)) o[dst / 8] |= (uint8_t)(0x80 >> (dst % 8));
        }
    d = o;
}

void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i))); }
}  // namespace

uint16_t crc16(const uint8_t* p, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    return (uint16_t)~crc;
}

uint16_t floatToHalf(float v) {
    uint32_t f; std::memcpy(&f, &v, 4);
    const uint32_t sign = (f >> 16) & 0x8000;
    const int exp = (int)((f >> 23) & 0xFF) - 127 + 15;
    uint32_t man = f & 0x7FFFFF;
    if (exp >= 31) return (uint16_t)(sign | 0x7BFF);   // clamp to the largest finite half
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;
        man |= 0x800000;
        return (uint16_t)(sign | ((man >> (14 - exp)) + ((man >> (13 - exp)) & 1)));
    }
    uint32_t h = sign | ((uint32_t)exp << 10) | (man >> 13);
    if (man & 0x1000) h++;
    return (uint16_t)h;
}

float halfToFloat(uint16_t h) {
    const int e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float v = e == 0 ? std::ldexp((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp((float)(m | 0x400), e - 25);
    return (h & 0x8000) ? -v : v;
}

bool Builder::addId(const std::string& call, int ssid, int icon) {
    if (call.empty() || call.size() > 252 || semi.size() + 5 + call.size() > (size_t)kMaxLen - 2) return false;
    semi.push_back(0); semi.push_back((uint8_t)(3 + call.size())); put16(semi, (uint16_t)icon);
    semi.insert(semi.end(), call.begin(), call.end()); semi.push_back((uint8_t)ssid);
    return true;
}
bool Builder::addTime(uint64_t t) {
    if (semi.size() + 7 > (size_t)kMaxLen - 2) return false;
    semi.push_back(1); semi.push_back(5);
    for (int i = 0; i < 5; i++) semi.push_back((uint8_t)(t >> (8 * i)));
    return true;
}
bool Builder::addGps(double lat, double lon, double altM, int errM, double headingDeg, double speedMps) {
    if (semi.size() + 16 > (size_t)kMaxLen - 2) return false;
    semi.push_back(2); semi.push_back(14);
    put32(semi, (uint32_t)(int32_t)std::lround(lat * 2147483648.0 / 90.0));
    put32(semi, (uint32_t)(int32_t)std::lround(lon * 2147483648.0 / 180.0));
    put16(semi, floatToHalf((float)altM));
    semi.push_back((uint8_t)errM);
    semi.push_back((uint8_t)std::lround(headingDeg * M_PI / 180.0 * 128.0 / M_PI) );
    put16(semi, floatToHalf((float)speedMps));
    return true;
}
bool Builder::addComment(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        const size_t n = std::min<size_t>(255, text.size() - i);
        if (semi.size() + 2 + n > (size_t)kMaxLen - 2) return false;
        semi.push_back(3); semi.push_back((uint8_t)n);
        semi.insert(semi.end(), text.begin() + i, text.begin() + i + n);
        i += n;
    }
    return true;
}

std::vector<uint8_t> encodeBody(const std::vector<uint8_t>& semi) {
    std::vector<uint8_t> d = semi;
    const uint16_t crc = crc16(d.data(), d.size());
    d.push_back((uint8_t)crc); d.push_back((uint8_t)(crc >> 8));
    whiten(d, d.size());
    const size_t len = d.size();
    std::vector<uint8_t> out = d;
    size_t i = 0;
    while (i < len) {
        const size_t rem = len - i;
        const Code& c = pickCode(rem);
        const size_t dl = c.k / 8;
        std::vector<uint8_t> chunk(dl, 0xAA), par(dl);
        std::memcpy(chunk.data(), d.data() + i, std::min(rem, dl));
        encodeChunk(chunk.data(), c, par.data());
        out.insert(out.end(), par.begin(), par.end());
        i += dl;
    }
    out.push_back((uint8_t)len); out.push_back((uint8_t)(len >> 8));
    interleave(out, false);
    return out;
}

std::vector<uint8_t> encodeFrame(const std::vector<uint8_t>& semi) {
    std::vector<uint8_t> f = {0x55, 0x55, 0x55, 0x55, 0xAB, 0xCD, 0xEF, 0x12};
    std::vector<uint8_t> b = encodeBody(semi);
    f.push_back((uint8_t)b.size()); f.push_back((uint8_t)(b.size() >> 8));
    f.insert(f.end(), b.begin(), b.end());
    return f;
}

bool parseWhiskers(const uint8_t* p, size_t n, Packet& o) {
    std::string comment;
    for (size_t i = 0; i + 2 <= n;) {
        const uint8_t type = p[i], len = p[i + 1];
        if (i + 2 + len > n) return false;
        const uint8_t* w = p + i + 2;
        o.whiskers++;
        if (type == 0 && len >= 3 && !o.hasId) {
            o.hasId = true; o.icon = w[0] | (w[1] << 8);
            o.callsign.assign((const char*)w + 2, len - 3); o.ssid = w[len - 1];
        } else if (type == 1 && len >= 5 && !o.hasTime) {
            o.hasTime = true; o.unixTime = 0;
            for (int k = 0; k < 5; k++) o.unixTime |= (uint64_t)w[k] << (8 * k);
        } else if (type == 2 && len >= 14 && !o.hasGps) {
            o.hasGps = true;
            const int32_t la = (int32_t)(w[0] | (w[1] << 8) | (w[2] << 16) | ((uint32_t)w[3] << 24));
            const int32_t lo = (int32_t)(w[4] | (w[5] << 8) | (w[6] << 16) | ((uint32_t)w[7] << 24));
            o.lat = la * 90.0 / 2147483648.0; o.lon = lo * 180.0 / 2147483648.0;
            o.altM = halfToFloat((uint16_t)(w[8] | (w[9] << 8)));
            o.errM = w[10]; o.headingDeg = w[11] * 180.0 / 128.0;
            o.speedMps = halfToFloat((uint16_t)(w[12] | (w[13] << 8)));
        } else if (type == 3) {
            comment.append((const char*)w, len);
        }
        i += 2 + len;
    }
    o.comment = comment;
    return true;
}

bool decodeBody(const std::vector<uint8_t>& body, Packet& out) {
    if (body.size() <= 4) return false;
    std::vector<uint8_t> d = body;
    interleave(d, true);
    const size_t len = d[d.size() - 2] | (d[d.size() - 1] << 8);
    if (len < 3 || len >= d.size()) return false;
    d.resize(len);   // data bits only; LDPC parity is not used to correct yet
    whiten(d, len);
    const uint16_t want = (uint16_t)(d[len - 2] | (d[len - 1] << 8));
    out = Packet();
    out.dataFecOk = crc16(d.data(), len - 2) == want;
    if (!out.dataFecOk) return false;
    return parseWhiskers(d.data(), len - 2, out);
}

}  // namespace cats
}  // namespace dect2
