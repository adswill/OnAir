// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Yaesu System Fusion frame coding (see fusion_proto.h).
#include "dect2/fusion/fusion_proto.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace dect2::fusion {

namespace {

std::vector<uint8_t> ilv(const std::vector<uint8_t>& in, int rowLen, int unit, bool inverse) {
    const int items = (int)in.size() / unit;
    if (rowLen <= 0 || items % rowLen != 0) return in;
    const int depth = items / rowLen;
    std::vector<uint8_t> out(in.size());
    for (int k = 0; k < items; k++) {
        const int src = (k % depth) * rowLen + k / depth;   // output item k is input item src
        const int a = inverse ? k : src, b = inverse ? src : k;   // inverse: out[src] = in[k]
        for (int u = 0; u < unit; u++) out[(size_t)(b * unit + u)] = in[(size_t)(a * unit + u)];
    }
    return out;
}

void bytesToBits(const uint8_t* p, int nbytes, std::vector<uint8_t>& out) {
    for (int i = 0; i < nbytes; i++) for (int b = 7; b >= 0; b--) out.push_back((p[i] >> b) & 1);
}
void bitsToBytes(const uint8_t* bits, int nbits, uint8_t* out) {
    for (int i = 0; i < (nbits + 7) / 8; i++) out[i] = 0;
    for (int i = 0; i < nbits; i++) if (bits[i]) out[i / 8] |= (uint8_t)(0x80 >> (i % 8));
}
void putBits(std::vector<uint8_t>& out, uint32_t v, int n) { for (int b = n - 1; b >= 0; b--) out.push_back((v >> b) & 1); }
uint32_t getBits(const uint8_t* p, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v = (v << 1) | (p[i] & 1); return v; }

constexpr uint32_t kGolayGen = 0xC75;

int pop32(uint32_t v) { int n = 0; while (v) { v &= v - 1; n++; } return n; }

uint32_t golay23(uint16_t d) {
    uint32_t r = (uint32_t)d << 11;
    for (int b = 22; b >= 11; b--) if (r & (1u << b)) r ^= kGolayGen << (b - 11);
    return ((uint32_t)d << 11) | (r & 0x7FF);
}

struct GolayTable {
    std::array<uint32_t, 4096> cw{};
    GolayTable() { for (uint32_t d = 0; d < 4096; d++) cw[d] = golay24((uint16_t)d); }
};
const GolayTable& gtab() { static const GolayTable t; return t; }

} // namespace

void syncLevels(int8_t out[kSyncDibits]) {
    for (int i = 0; i < kSyncDibits; i++) out[i] = (int8_t)dibitLevel((int)((kSyncWord >> (38 - 2 * i)) & 3));
}

uint16_t crc16(const uint8_t* bits, int n) {
    uint16_t c = 0;
    for (int i = 0; i < n; i++) {
        const bool fb = ((c >> 15) & 1) ^ (bits[i] & 1);
        c = (uint16_t)(c << 1);
        if (fb) c ^= 0x1021;
    }
    return (uint16_t)~c;
}

uint32_t golay24(uint16_t d) {
    const uint32_t c = golay23(d & 0xFFF);
    return (c << 1) | (uint32_t)(pop32(c) & 1);
}

bool golay24Decode(uint32_t w, uint16_t& d, int& errors) {
    const GolayTable& t = gtab();
    int best = 99, bi = 0;
    for (int i = 0; i < 4096; i++) {
        const int dist = pop32((w ^ t.cw[(size_t)i]) & 0xFFFFFF);
        if (dist < best) { best = dist; bi = i; }
    }
    errors = best;
    d = (uint16_t)bi;
    return best <= 3;
}

std::vector<uint8_t> convEncode(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out;
    out.reserve(in.size() * 2);
    int h[5] = {0, 0, 0, 0, 0};   // h[k] = input k steps ago
    for (uint8_t b : in) {
        h[0] = b & 1;
        out.push_back((uint8_t)(h[0] ^ h[3] ^ h[4]));
        out.push_back((uint8_t)(h[0] ^ h[1] ^ h[2] ^ h[4]));
        for (int k = 4; k > 0; k--) h[k] = h[k - 1];
    }
    return out;
}

std::vector<uint8_t> convDecode(const std::vector<uint8_t>& coded, int& errors) {
    const int N = (int)coded.size() / 2;
    constexpr int S = 16;
    std::vector<int> pm(S, 1 << 20), npm(S);
    pm[0] = 0;
    std::vector<std::array<uint8_t, S>> from((size_t)N);   // survivor predecessor state per step
    std::vector<std::array<uint8_t, S>> inp((size_t)N);
    for (int t = 0; t < N; t++) {
        std::fill(npm.begin(), npm.end(), 1 << 20);
        for (int s = 0; s < S; s++) {
            if (pm[(size_t)s] >= (1 << 20)) continue;
            for (int u = 0; u < 2; u++) {
                const int h1 = s & 1, h2 = (s >> 1) & 1, h3 = (s >> 2) & 1, h4 = (s >> 3) & 1;   // state: bit k-1 = input k steps ago
                const int o1 = u ^ h3 ^ h4, o2 = u ^ h1 ^ h2 ^ h4;
                const int m = pm[(size_t)s] + (o1 != (coded[(size_t)(2 * t)] & 1)) + (o2 != (coded[(size_t)(2 * t + 1)] & 1));
                const int ns = ((s << 1) | u) & 15;
                if (m < npm[(size_t)ns]) { npm[(size_t)ns] = m; from[(size_t)t][(size_t)ns] = (uint8_t)s; inp[(size_t)t][(size_t)ns] = (uint8_t)u; }
            }
        }
        pm = npm;
    }
    errors = pm[0];
    std::vector<uint8_t> out((size_t)N);
    int s = 0;
    for (int t = N - 1; t >= 0; t--) { out[(size_t)t] = inp[(size_t)t][(size_t)s]; s = from[(size_t)t][(size_t)s]; }
    return out;
}

std::vector<uint8_t> interleaveDibits(const std::vector<uint8_t>& bits, int rowDibits) { return ilv(bits, rowDibits, 2, false); }
std::vector<uint8_t> deinterleaveDibits(const std::vector<uint8_t>& bits, int rowDibits) { return ilv(bits, rowDibits, 2, true); }

void whiten(uint8_t* bits, int n) {
    int s[9] = {1, 0, 0, 1, 0, 0, 1, 1, 1};   // s[k] = S0k; S08..S00 = 1 1 1 0 0 1 0 0 1
    for (int i = 0; i < n; i++) {
        bits[i] ^= (uint8_t)s[0];
        const int fb = s[0] ^ s[4];
        for (int k = 0; k < 8; k++) s[k] = s[k + 1];
        s[8] = fb;
    }
}

uint32_t fichPack(const Fich& f) {
    return ((uint32_t)(f.fi & 3) << 30) | ((uint32_t)(f.cs & 3) << 28) | ((uint32_t)(f.cm & 3) << 26) | ((uint32_t)(f.bn & 3) << 24) |
           ((uint32_t)(f.bt & 3) << 22) | ((uint32_t)(f.fn & 7) << 19) | ((uint32_t)(f.ft & 7) << 16) | ((uint32_t)(f.mr & 7) << 11) |
           ((uint32_t)(f.voip & 1) << 10) | ((uint32_t)(f.dt & 3) << 8) | ((uint32_t)(f.sqlOff & 1) << 7) | (uint32_t)(f.sqlCode & 127);
}
Fich fichUnpack(uint32_t w) {
    Fich f;
    f.fi = (w >> 30) & 3; f.cs = (w >> 28) & 3; f.cm = (w >> 26) & 3; f.bn = (w >> 24) & 3; f.bt = (w >> 22) & 3;
    f.fn = (w >> 19) & 7; f.ft = (w >> 16) & 7; f.mr = (w >> 11) & 7; f.voip = (w >> 10) & 1; f.dt = (w >> 8) & 3;
    f.sqlOff = (w >> 7) & 1; f.sqlCode = w & 127;
    return f;
}

std::vector<uint8_t> fichEncode(const Fich& f) {
    std::vector<uint8_t> b;
    putBits(b, fichPack(f), 32);
    putBits(b, crc16(b.data(), 32), 16);
    std::vector<uint8_t> g;
    for (int i = 0; i < 4; i++) putBits(g, golay24((uint16_t)getBits(&b[(size_t)(12 * i)], 12)), 24);
    for (int i = 0; i < 4; i++) g.push_back(0);
    return interleaveDibits(convEncode(g), 5);
}

bool fichDecode(const uint8_t* bits200, Fich& f, int& fixes) {
    std::vector<uint8_t> c(bits200, bits200 + 200);
    int err = 0;
    const std::vector<uint8_t> g = convDecode(deinterleaveDibits(c, 5), err);
    fixes = err;
    std::vector<uint8_t> d;
    for (int i = 0; i < 4; i++) {
        uint16_t v = 0; int e = 0;
        if (!golay24Decode(getBits(&g[(size_t)(24 * i)], 24), v, e)) return false;
        fixes += e;
        putBits(d, v, 12);
    }
    if (crc16(d.data(), 32) != getBits(&d[32], 16)) return false;
    f = fichUnpack(getBits(d.data(), 32));
    return true;
}

std::vector<uint8_t> dchEncode(const uint8_t* bytes, int nbytes) {
    std::vector<uint8_t> b;
    bytesToBits(bytes, nbytes, b);
    whiten(b.data(), nbytes * 8);
    putBits(b, crc16(b.data(), nbytes * 8), 16);
    for (int i = 0; i < 4; i++) b.push_back(0);
    return interleaveDibits(convEncode(b), nbytes == 20 ? 9 : 5);
}

bool dchDecode(const uint8_t* bits, int nbits, uint8_t* bytes, int& fixes) {
    const int nbytes = nbits == 360 ? 20 : 10;
    std::vector<uint8_t> c(bits, bits + nbits);
    const std::vector<uint8_t> d = convDecode(deinterleaveDibits(c, nbytes == 20 ? 9 : 5), fixes);
    const int n = nbytes * 8;
    if (crc16(d.data(), n) != getBits(&d[(size_t)n], 16)) return false;
    std::vector<uint8_t> w(d.begin(), d.begin() + n);
    whiten(w.data(), n);
    bitsToBytes(w.data(), n, bytes);
    return true;
}

std::vector<uint8_t> vd2VoiceEncode(const uint8_t* v) {
    std::vector<uint8_t> b;
    for (int i = 0; i < 27; i++) for (int k = 0; k < 3; k++) b.push_back(v[i] & 1);
    for (int i = 27; i < 49; i++) b.push_back(v[i] & 1);
    b.push_back(0);
    whiten(b.data(), 104);
    return ilv(b, 26, 1, false);
}

void vd2VoiceDecode(const uint8_t* bits104, uint8_t* out) {
    std::vector<uint8_t> in(bits104, bits104 + 104);
    std::vector<uint8_t> b = ilv(in, 26, 1, true);
    whiten(b.data(), 104);
    for (int i = 0; i < 27; i++) out[i] = (uint8_t)((b[(size_t)(3 * i)] + b[(size_t)(3 * i + 1)] + b[(size_t)(3 * i + 2)]) >= 2);
    for (int i = 0; i < 22; i++) out[27 + i] = b[(size_t)(81 + i)];
}

bool buildFrameBits(const FrameContent& c, std::vector<uint8_t>& out) {
    out.clear();
    putBits(out, (uint32_t)(kSyncWord >> 8), 32);
    putBits(out, (uint32_t)(kSyncWord & 0xFF), 8);
    const std::vector<uint8_t> fich = fichEncode(c.fich);
    out.insert(out.end(), fich.begin(), fich.end());
    const bool hdr = c.fich.fi == 0 || c.fich.fi == 2;
    auto unitBits = [&](size_t i, int nbytes) { return i < c.units.size() && (int)c.units[i].size() == nbytes ? dchEncode(c.units[i].data(), nbytes) : std::vector<uint8_t>(); };
    if (hdr || c.fich.dt == 1) {
        const std::vector<uint8_t> u0 = unitBits(0, 20), u1 = unitBits(1, 20);
        if (u0.empty() || u1.empty()) return false;
        for (int i = 0; i < 5; i++) {
            out.insert(out.end(), u0.begin() + 72 * i, u0.begin() + 72 * (i + 1));
            out.insert(out.end(), u1.begin() + 72 * i, u1.begin() + 72 * (i + 1));
        }
    } else if (c.fich.dt == 0) {
        const std::vector<uint8_t> u = unitBits(0, 20);
        if (u.empty() || c.voice.size() != 5) return false;
        for (int i = 0; i < 5; i++) {
            if (c.voice[(size_t)i].bytes.size() != 9) return false;
            out.insert(out.end(), u.begin() + 72 * i, u.begin() + 72 * (i + 1));
            bytesToBits(c.voice[(size_t)i].bytes.data(), 9, out);
        }
    } else if (c.fich.dt == 2) {
        const std::vector<uint8_t> u = unitBits(0, 10);
        if (u.empty() || c.voice.size() != 5) return false;
        for (int i = 0; i < 5; i++) {
            if (c.voice[(size_t)i].bytes.size() != 7) return false;
            std::vector<uint8_t> v;
            bytesToBits(c.voice[(size_t)i].bytes.data(), 7, v);
            const std::vector<uint8_t> e = vd2VoiceEncode(v.data());
            out.insert(out.end(), u.begin() + 40 * i, u.begin() + 40 * (i + 1));
            out.insert(out.end(), e.begin(), e.end());
        }
    } else {
        const bool cc0 = c.fich.fn == 0 && c.fich.ft == 1;
        size_t vi = 0;
        if (cc0) {
            const std::vector<uint8_t> u = unitBits(0, 20);
            if (u.empty() || c.voice.size() != 2) return false;
            out.insert(out.end(), u.begin(), u.end());
            for (int i = 0; i < 72; i++) out.push_back(0);   // reserved
        } else if (c.voice.size() != 5) return false;
        for (; vi < c.voice.size(); vi++) {
            if (c.voice[vi].bytes.size() != 18) return false;
            bytesToBits(c.voice[vi].bytes.data(), 18, out);
        }
    }
    return (int)out.size() == kFrameBits;
}

bool parseFrameBits(const uint8_t* b, FrameContent& c) {
    c.units.clear(); c.unitOk.clear(); c.voice.clear();
    c.fichFixes = 0;
    c.fichOk = fichDecode(b + 40, c.fich, c.fichFixes);
    if (!c.fichOk) return false;
    const uint8_t* p = b + 240;
    auto addUnit = [&](const std::vector<uint8_t>& coded, int nbytes) {
        std::vector<uint8_t> by((size_t)nbytes);
        int fx = 0;
        const bool ok = dchDecode(coded.data(), (int)coded.size(), by.data(), fx);
        c.units.push_back(by);
        c.unitOk.push_back(ok);
    };
    const bool hdr = c.fich.fi == 0 || c.fich.fi == 2;
    if (hdr || c.fich.dt == 1) {
        std::vector<uint8_t> u0, u1;
        for (int i = 0; i < 5; i++) {
            u0.insert(u0.end(), p + 144 * i, p + 144 * i + 72);
            u1.insert(u1.end(), p + 144 * i + 72, p + 144 * i + 144);
        }
        addUnit(u0, 20); addUnit(u1, 20);
    } else if (c.fich.dt == 0) {
        std::vector<uint8_t> u;
        for (int i = 0; i < 5; i++) {
            u.insert(u.end(), p + 144 * i, p + 144 * i + 72);
            VoiceUnit v; v.kind = 1; v.bytes.assign(9, 0);
            bitsToBytes(p + 144 * i + 72, 72, v.bytes.data());
            c.voice.push_back(v);
        }
        addUnit(u, 20);
    } else if (c.fich.dt == 2) {
        std::vector<uint8_t> u;
        for (int i = 0; i < 5; i++) {
            u.insert(u.end(), p + 144 * i, p + 144 * i + 40);
            uint8_t v49[49];
            vd2VoiceDecode(p + 144 * i + 40, v49);
            VoiceUnit v; v.kind = 2; v.bytes.assign(7, 0);
            bitsToBytes(v49, 49, v.bytes.data());
            c.voice.push_back(v);
        }
        addUnit(u, 10);
    } else {
        int first = 0;
        if (c.fich.fn == 0 && c.fich.ft == 1) {
            addUnit(std::vector<uint8_t>(p, p + 360), 20);
            first = 432;   // 360 DCH + 72 reserved
            for (int i = 0; i < 2; i++) {
                VoiceUnit v; v.kind = 3; v.bytes.assign(18, 0);
                bitsToBytes(p + first + 144 * i, 144, v.bytes.data());
                c.voice.push_back(v);
            }
        } else {
            for (int i = 0; i < 5; i++) {
                VoiceUnit v; v.kind = 3; v.bytes.assign(18, 0);
                bitsToBytes(p + 144 * i, 144, v.bytes.data());
                c.voice.push_back(v);
            }
        }
    }
    return true;
}

std::string trimField(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) s += (p[i] >= 32 && p[i] < 127) ? (char)p[i] : (p[i] == 0 ? ' ' : '.');
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

void padField(const std::string& s, uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) p[i] = i < s.size() ? (uint8_t)s[i] : (uint8_t)' ';
}

} // namespace dect2::fusion
