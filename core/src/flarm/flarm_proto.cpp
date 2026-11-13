// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FLARM V6 packet: see flarm_proto.h. The constants and the layout follow the open SoftRF (Legacy) and OGN descriptions of the old protocol.
#include "dect2/flarm/flarm_proto.h"
#include <cmath>
#include <cstring>
#include "dect2/common/crc.h"
#include "dect2/common/bits.h"
#include "dect2/common/linecode.h"
#include "dect2/common/mathconst.h"
#include "dect2/common/endian.h"

namespace dect2 {
namespace flarm {

namespace {
constexpr uint32_t kKey1[8] = {0xe43276df, 0xdca83759, 0x9802b8ac, 0x4675a56b, 0xfc78ea65, 0x804b90ea, 0xb76542cd, 0x329dfa32};
constexpr uint32_t kKey2 = 0x045d9f3b, kKey3 = 0x87b562f4;
constexpr uint32_t kDelta = 0x9e3779b9;
using dect2::math::kPi;

inline uint32_t rd32(const uint8_t* p) { return (uint32_t)dect2::endian::loadLe32(p); }
inline void wr32(uint8_t* p, uint32_t v) { dect2::endian::storeLe32(p, v); }
inline int parity8(uint8_t b) { return dect2::bits::parity((uint32_t)b); }
int signExt(uint32_t v, int bits) { return (int)(v << (32 - bits)) >> (32 - bits); }
}

uint16_t crc16(const uint8_t* d, size_t n) { return crc::crc16CcittFalse(d, n); }

void btea(uint32_t* v, int n, const uint32_t key[4], int rounds) {
    uint32_t y, z, sum, p, e;
#define MX (((z >> 5 ^ y << 2) + (y >> 3 ^ z << 4)) ^ ((sum ^ y) + (key[(p & 3) ^ e] ^ z)))
    if (n > 1) {
        uint32_t r = rounds ? (uint32_t)rounds : 6u + 52u / (uint32_t)n;
        sum = 0; z = v[n - 1];
        do {
            sum += kDelta; e = (sum >> 2) & 3;
            for (p = 0; p < (uint32_t)n - 1; p++) { y = v[p + 1]; z = v[p] += MX; }
            y = v[0]; z = v[n - 1] += MX;
        } while (--r);
    } else if (n < -1) {
        n = -n;
        uint32_t r = rounds ? (uint32_t)rounds : 6u + 52u / (uint32_t)n;
        sum = r * kDelta; y = v[0];
        do {
            e = (sum >> 2) & 3;
            for (p = (uint32_t)n - 1; p > 0; p--) { z = v[p - 1]; y = v[p] -= MX; }
            z = v[n - 1]; y = v[0] -= MX;
            sum -= kDelta;
        } while (--r);
    }
#undef MX
}

uint32_t obscure(uint32_t key, uint32_t seed) {
    const uint32_t m1 = seed * (key ^ (key >> 16));
    const uint32_t m2 = seed * (m1 ^ (m1 >> 16));
    return m2 ^ (m2 >> 16);
}

void makeV6Key(uint32_t key[4], uint32_t ts, uint32_t addrShifted) {
    for (int i = 0; i < 4; i++) {
        const int ndx = ((ts >> 23) & 1) ? i + 4 : i;
        key[i] = obscure(kKey1[ndx] ^ ((ts >> 6) ^ addrShifted), kKey2) ^ kKey3;
    }
}

int packetType(const uint8_t in[kPayloadBytes]) { return (int)((rd32(in) >> 24) & 15); }
uint32_t packetAddr(const uint8_t in[kPayloadBytes]) { return rd32(in) & 0xFFFFFF; }

void encodeV6(const V6& p, uint32_t ts, uint8_t out[kPayloadBytes]) {
    uint32_t w[6];
    w[0] = (p.addr & 0xFFFFFF) | ((uint32_t)(p.type & 15) << 24) | ((uint32_t)(p.addrType & 7) << 28);
    w[1] = ((uint32_t)p.vsRaw & 0x3FF) | ((uint32_t)p.airborne << 12) | ((uint32_t)p.stealth << 13) | ((uint32_t)p.noTrack << 14)
         | ((uint32_t)(p.gps & 0xFFF) << 16) | ((uint32_t)(p.aircraftType & 15) << 28);
    w[2] = (p.lat19 & 0x7FFFF) | ((p.alt & 0x1FFF) << 19);
    w[3] = (p.lon20 & 0xFFFFF) | ((uint32_t)(p.smult & 3) << 30);
    w[4] = (uint32_t)(uint8_t)p.ns[0] | ((uint32_t)(uint8_t)p.ns[1] << 8) | ((uint32_t)(uint8_t)p.ns[2] << 16) | ((uint32_t)(uint8_t)p.ns[3] << 24);
    w[5] = (uint32_t)(uint8_t)p.ew[0] | ((uint32_t)(uint8_t)p.ew[1] << 8) | ((uint32_t)(uint8_t)p.ew[2] << 16) | ((uint32_t)(uint8_t)p.ew[3] << 24);
    uint8_t b[kPayloadBytes];
    for (int i = 0; i < 6; i++) wr32(b + 4 * i, w[i]);
    int par = 0;
    for (int i = 0; i < kPayloadBytes; i++) par ^= parity8(b[i]);
    if (par) { w[1] |= 1u << 15; wr32(b + 4, w[1]); }
    uint32_t key[4];
    makeV6Key(key, ts, (p.addr << 8) & 0xFFFFFF);
    uint32_t e[5];
    for (int i = 0; i < 5; i++) e[i] = rd32(b + 4 + 4 * i);
    btea(e, 5, key, 6);
    wr32(b, w[0]);
    for (int i = 0; i < 5; i++) wr32(b + 4 + 4 * i, e[i]);
    std::memcpy(out, b, kPayloadBytes);
}

DecodeResult decodeV6(const uint8_t in[kPayloadBytes], uint32_t ts, V6& o) {
    const uint32_t w0 = rd32(in);
    if (((w0 >> 24) & 15) != 0) return DecNotV6;
    uint32_t key[4];
    makeV6Key(key, ts, (w0 << 8) & 0xFFFFFF);
    uint32_t e[5];
    for (int i = 0; i < 5; i++) e[i] = rd32(in + 4 + 4 * i);
    btea(e, -5, key, 6);
    uint8_t b[kPayloadBytes];
    wr32(b, w0);
    for (int i = 0; i < 5; i++) wr32(b + 4 + 4 * i, e[i]);
    int par = 0;
    for (int i = 0; i < kPayloadBytes; i++) par ^= parity8(b[i]);
    if (par) return DecBadParity;
    const uint32_t w1 = e[0], w2 = e[1], w3 = e[2], w4 = e[3], w5 = e[4];
    o = V6();
    o.addr = w0 & 0xFFFFFF; o.type = (int)((w0 >> 24) & 15); o.addrType = (int)((w0 >> 28) & 7);
    o.vsRaw = signExt(w1 & 0x3FF, 10);
    o.airborne = (w1 >> 12) & 1; o.stealth = (w1 >> 13) & 1; o.noTrack = (w1 >> 14) & 1;
    o.gps = (int)((w1 >> 16) & 0xFFF); o.aircraftType = (int)((w1 >> 28) & 15);
    o.lat19 = w2 & 0x7FFFF; o.alt = w2 >> 19;
    o.lon20 = w3 & 0xFFFFF; o.smult = (int)(w3 >> 30);
    for (int i = 0; i < 4; i++) { o.ns[i] = (int8_t)(w4 >> (8 * i)); o.ew[i] = (int8_t)(w5 >> (8 * i)); }
    return DecOk;
}

void packPosition(double latDeg, double lonDeg, uint32_t& lat19, uint32_t& lon20) {
    const int32_t lat = (int32_t)std::lround(latDeg * 1e7), lon = (int32_t)std::lround(lonDeg * 1e7);
    lat19 = (uint32_t)(((lat >> 7) + ((lat & 0x40) ? (lat < 0 ? -1 : 1) : 0)) & 0x7FFFF);
    lon20 = (uint32_t)(((lon >> 7) + ((lon & 0x40) ? (lon < 0 ? -1 : 1) : 0)) & 0xFFFFF);
}

void unpackPosition(uint32_t lat19, uint32_t lon20, double refLat, double refLon, double& latDeg, double& lonDeg) {
    const int32_t rl = (int32_t)std::lround(refLat * 1e7) >> 7, ro = (int32_t)std::lround(refLon * 1e7) >> 7;
    int32_t la = (int32_t)(((uint32_t)((int32_t)lat19 - rl)) & 0x7FFFF);
    if (la >= 0x40000) la -= 0x80000;
    int32_t lo = (int32_t)(((uint32_t)((int32_t)lon20 - ro)) & 0xFFFFF);
    if (lo >= 0x80000) lo -= 0x100000;
    latDeg = (double)((la + rl) * 128) / 1e7;
    lonDeg = (double)((lo + ro) * 128) / 1e7;
}

void unpackVelocity(const V6& p, double& speedMs, double& trackDeg, double& climbMs) {
    const int ns = (p.ns[0] + p.ns[1] + p.ns[2] + p.ns[3]) / 4, ew = (p.ew[0] + p.ew[1] + p.ew[2] + p.ew[3]) / 4;
    speedMs = std::sqrt((double)(ew * ew + ns * ns)) * (double)(1 << p.smult) / 4.0;
    trackDeg = 0;
    if (ns || ew) { trackDeg = std::atan2((double)ew, (double)ns) * 180.0 / kPi; if (trackDeg < 0) trackDeg += 360.0; }
    climbMs = (double)(p.vsRaw * (1 << p.smult)) / 10.0;
}

void packVelocity(double speedMs, double trackDeg, double climbMs, V6& p) {
    int s4 = (int)std::lround(speedMs * 4.0);
    if (s4 > 0x3FF) s4 = 0x3FF;
    p.smult = (s4 & 0x200) ? 3 : (s4 & 0x100) ? 2 : (s4 & 0x080) ? 1 : 0;
    const int speed = s4 >> p.smult;
    const double a = trackDeg * kPi / 180.0;
    const int8_t ns = (int8_t)(int)(speed * std::cos(a)), ew = (int8_t)(int)(speed * std::sin(a));
    for (int i = 0; i < 4; i++) { p.ns[i] = ns; p.ew[i] = ew; }
    p.vsRaw = ((int)std::lround(climbMs * 10.0)) >> p.smult;
    if (p.vsRaw > 511) p.vsRaw = 511;
    if (p.vsRaw < -512) p.vsRaw = -512;
}

const char* aircraftTypeName(int t) {
    static const char* n[16] = {"unknown", "glider", "tow plane", "helicopter", "skydiver", "drop plane", "hang glider", "paraglider",
                                "powered aircraft", "jet", "gyrocopter", "balloon", "airship", "UAV", "reserved", "static object"};
    return n[t & 15];
}
const char* addrTypeName(int t) {
    static const char* n[4] = {"random", "ICAO", "FLARM", "anonymous"};
    return n[t & 3];
}

std::vector<uint8_t> manchester(const uint8_t* bytes, size_t n) { return dect2::linecode::manchesterEncode(bytes, n); }

std::vector<uint8_t> frameToChips(const uint8_t payload[kPayloadBytes], bool withSync) {
    uint8_t f[kFrameBytes];
    std::memcpy(f, payload, kPayloadBytes);
    const uint16_t c = crc16(f, kPayloadBytes);
    f[24] = (uint8_t)(c >> 8); f[25] = (uint8_t)c;
    std::vector<uint8_t> chips;
    if (withSync) {
        const uint8_t s[4] = {0xF5, 0x31, 0xFA, 0xB6};
        chips = manchester(s, 4);
    }
    const std::vector<uint8_t> d = manchester(f, kFrameBytes);
    chips.insert(chips.end(), d.begin(), d.end());
    return chips;
}

bool checkCrc(const uint8_t f[kFrameBytes]) {
    const uint16_t c = crc16(f, kPayloadBytes);
    return (f[24] == (uint8_t)(c >> 8) && f[25] == (uint8_t)c) || (f[25] == (uint8_t)(c >> 8) && f[24] == (uint8_t)c);
}

} // namespace flarm
} // namespace dect2
