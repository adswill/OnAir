// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Chinese radiosondes of the GTH family (Huayuntianyi HT03G-1U / GTH3 and the GTH6 of the Changwang "Feng" series): 2400 baud GFSK, 109 byte frames
// of a 5 byte preamble (AA), the sync 2D D4 63 and 101 data bytes, bits sent least significant first, a CRC-16 (reflected 0x1021, start value 0) over
// the 97 bytes after the sync, little endian GPS position as 32 bit floats in radians. Frame layout from zilog80's RS demodulators
// (cf06ht03mod.c, github.com/rs1729/RS, GPL-3.0); this is our own code written from that format. The CF-06-AH frame (Reed-Solomon, GPS week) of the
// same file is not decoded. Validated on simulated frames only: no recording of a GTH sonde was available. Like the reference decoder this sends the
// position and speed, and temperature, humidity and pressure when the fields hold plausible values (what each field means for the 2026 GTH6 batch is
// not confirmed).
#include "dect2/sonde/sonde_bits.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace dect2 {

namespace {

constexpr int kFrameLen = 109, kOfs = 5, kHeadBytes = 8, kDataBits = (kFrameLen - kHeadBytes) * 8;

uint16_t gthCrc(const uint8_t* d, int n) {
    uint32_t rem = 0;
    for (int i = 0; i < n; i++) {
        rem ^= d[i];
        for (int j = 0; j < 8; j++) rem = (rem & 1) ? (rem >> 1) ^ 0x8408 : (rem >> 1);
    }
    return (uint16_t)(rem & 0xFFFF);
}

uint64_t headerWord() {   // the first 64 bits on air: AA AA AA AA AA 2D D4 63, each byte least significant bit first (oldest bit in the top)
    static const uint8_t h[kHeadBytes] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0x2D, 0xD4, 0x63};
    uint64_t w = 0;
    for (int i = 0; i < kHeadBytes; i++) for (int b = 0; b < 8; b++) w = (w << 1) | ((h[i] >> b) & 1);
    return w;
}

float f32of(const uint8_t* p) { float f; std::memcpy(&f, p, 4); return f; }
int32_t i32le(const uint8_t* p) { return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)); }
int16_t i16le(const uint8_t* p) { return (int16_t)(p[0] | (p[1] << 8)); }

double utcOfDay(int h, int m, double sec, int64_t nowUnix) {   // the sonde sends no date: the nearest day wins
    const int64_t day = nowUnix - nowUnix % 86400;
    double t = (double)day + h * 3600.0 + m * 60.0 + sec;
    if (t - nowUnix > 43200) t -= 86400; else if (nowUnix - t > 43200) t += 86400;
    return t;
}

class GthDecoder : public SondeBitDecoder {
public:
    const char* type() const override { return "GTH"; }
    double symbolRate() const override { return 2400; }
    bool midpointSlicer() const override { return true; }   // the short preamble does not settle the mean: slice between the extremes
    void reset() override { hdr_ = 0; run_ = false; bits_.clear(); }
    void push(const uint8_t* sym, size_t n, double, std::vector<SondeFix>& out) override {
        for (size_t i = 0; i < n; i++) {
            if (!run_) {
                hdr_ = (hdr_ << 1) | (uint64_t)(sym[i] & 1);
                // the last 32 bits (AA 2D D4 63) decide: the 40 bit preamble is short and its first bits are often lost while the slicer settles
                const uint64_t v = (hdr_ ^ headerWord()) & 0xFFFFFFFFull;
                if (__builtin_popcountll(v) <= 3) { run_ = true; inv_ = false; bits_.clear(); }
                else if (__builtin_popcountll(~v & 0xFFFFFFFFull) <= 3) { run_ = true; inv_ = true; bits_.clear(); }
                continue;
            }
            bits_.push_back((uint8_t)((sym[i] & 1) ^ (inv_ ? 1 : 0)));
            if ((int)bits_.size() == kDataBits) { run_ = false; hdr_ = 0; frame(out); }
        }
    }
private:
    uint64_t hdr_ = 0; bool run_ = false, inv_ = false;
    std::vector<uint8_t> bits_;
    void frame(std::vector<SondeFix>& out) {
        uint8_t f[kFrameLen] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0x2D, 0xD4, 0x63};
        for (int i = 0; i < kDataBits; i++) if (bits_[(size_t)i]) f[kHeadBytes + i / 8] |= (uint8_t)(1 << (i % 8));
        SondeFix o;
        if (gthParseFrame(f, kFrameLen, o)) out.push_back(o);
    }
};

} // namespace

bool gthParseFrame(const uint8_t* f, size_t n, SondeFix& o) {
    if (n < (size_t)kFrameLen) return false;
    const uint8_t* p = f + kOfs;
    if (p[5] == 0xAA && p[6] == 0xAA) return false;   // the CF-06-AH frame: not decoded here
    const uint16_t crcDat = (uint16_t)(p[100] | (p[101] << 8));
    if (crcDat != gthCrc(p + 3, 97)) return false;
    const uint32_t sn = ((uint32_t)p[7] << 24) | ((uint32_t)p[8] << 16) | ((uint32_t)p[9] << 8) | p[10];
    const int frnr = (p[97] << 8) | p[98];
    if (frnr == 0 && sn == 0) return false;   // an all-zero frame has a valid CRC
    const int hh = p[12] & 0x1F, mm = p[13] & 0x3F;
    const double ss = (p[14] | ((p[13] & 0xC0) << 2)) / 10.0;
    const double lon = f32of(p + 16) * 180.0 / M_PI, lat = f32of(p + 20) * 180.0 / M_PI, alt = i32le(p + 24) / 10.0;
    if (!(std::fabs(lat) <= 90) || !(std::fabs(lon) <= 180) || hh > 23 || mm > 59 || ss >= 61) return false;
    const double vN = i16le(p + 28) / 100.0, vE = i16le(p + 30) / 100.0, vU = i16le(p + 32) / 100.0;
    char id[24]; snprintf(id, sizeof id, "GTH-%08X", sn);
    o = SondeFix();
    o.type = "GTH"; o.subtype = "GTH"; o.serial = id; o.frame = frnr; o.crcOk = true;
    o.hasPos = true; o.lat = lat; o.lon = lon; o.altM = alt;
    o.hasVel = true; o.hSpeed = std::sqrt(vN * vN + vE * vE); o.vSpeed = vU;
    o.headingDeg = std::atan2(vE, vN) * 180.0 / M_PI; if (o.headingDeg < 0) o.headingDeg += 360;
    o.hasTime = true; o.unixTime = utcOfDay(hh, mm, ss, (int64_t)time(nullptr));
    const double T = i16le(p + 84) / 100.0, RH = i16le(p + 88) / 100.0, P = (double)(p[90] | (p[91] << 8) | (p[92] << 16)) / 100.0;
    if (T > -273.0 && T < 100.0) { o.hasTemp = true; o.tempC = T; }
    if (RH > -0.5 && RH <= 100.5) { o.hasHumidity = true; o.humidity = RH; }
    if (P > 0.0 && P < 2000.0) { o.hasPressure = true; o.pressureHpa = P; }
    return true;
}

std::vector<uint8_t> gthFrameBytes(const SondeTruth& t, int frame, uint32_t sn) {
    std::vector<uint8_t> v(kFrameLen, 0);
    uint8_t* f = v.data();
    for (int i = 0; i < 5; i++) f[i] = 0xAA;
    f[5] = 0x2D; f[6] = 0xD4; f[7] = 0x63;
    uint8_t* p = f + kOfs;
    // the fields this decoder does not read get a pseudo-random fill: a run of hundreds of equal bits would drain the receiver's DC tracking, which a
    // real frame (all fields filled) does not do
    uint32_t x = 0x9E3779B9u ^ ((uint32_t)frame * 2654435761u) ^ sn;
    for (int i = 3; i < 100; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; p[i] = (uint8_t)(x >> 8); }
    p[3] = 0x7F; p[4] = 0xFF; p[5] = 0xFF; p[6] = 0xEE;
    for (int i = 0; i < 4; i++) p[7 + i] = (uint8_t)(sn >> (24 - 8 * i));
    const int64_t s = (int64_t)std::floor(t.unixTime);
    const int tenths = (int)std::lround((t.unixTime - (double)s + (double)(s % 60)) * 10.0);   // seconds of the minute, tenths
    p[12] = (uint8_t)((s % 86400) / 3600);
    p[13] = (uint8_t)(((s % 3600) / 60) | ((tenths >> 8) << 6));
    p[14] = (uint8_t)(tenths & 0xFF);
    const float lon = (float)(t.lon * M_PI / 180.0), lat = (float)(t.lat * M_PI / 180.0);
    std::memcpy(p + 16, &lon, 4); std::memcpy(p + 20, &lat, 4);
    const int32_t alt = (int32_t)std::lround(t.altM * 10.0);
    for (int i = 0; i < 4; i++) p[24 + i] = (uint8_t)((uint32_t)alt >> (8 * i));
    const double hd = t.headingDeg * M_PI / 180.0;
    const int16_t vals[3] = {(int16_t)std::lround(t.hSpeed * std::cos(hd) * 100), (int16_t)std::lround(t.hSpeed * std::sin(hd) * 100), (int16_t)std::lround(t.vSpeed * 100)};
    for (int k = 0; k < 3; k++) { p[28 + 2 * k] = (uint8_t)(vals[k] & 0xFF); p[29 + 2 * k] = (uint8_t)((uint16_t)vals[k] >> 8); }
    const int16_t T = (int16_t)std::lround(t.tempC * 100), RH = (int16_t)std::lround(t.humidity * 100);
    const int32_t P = (int32_t)std::lround(t.pressureHpa * 100);
    p[84] = (uint8_t)(T & 0xFF); p[85] = (uint8_t)((uint16_t)T >> 8);
    p[88] = (uint8_t)(RH & 0xFF); p[89] = (uint8_t)((uint16_t)RH >> 8);
    p[90] = (uint8_t)(P & 0xFF); p[91] = (uint8_t)((P >> 8) & 0xFF); p[92] = (uint8_t)((P >> 16) & 0xFF);
    p[97] = (uint8_t)(frame >> 8); p[98] = (uint8_t)(frame & 0xFF);
    const uint16_t c = gthCrc(p + 3, 97);
    p[100] = (uint8_t)(c & 0xFF); p[101] = (uint8_t)(c >> 8);
    return v;
}

std::vector<uint8_t> gthSymbols(const SondeTruth& t, int frame, uint32_t sn) {
    const std::vector<uint8_t> f = gthFrameBytes(t, frame, sn);
    std::vector<uint8_t> s;
    s.reserve(f.size() * 8 + 16);
    for (uint8_t b : f) for (int i = 0; i < 8; i++) s.push_back((b >> i) & 1);
    for (int i = 0; i < 8; i++) s.push_back((uint8_t)(i & 1));   // a short tail so the last bit is clocked out
    return s;
}

std::unique_ptr<SondeBitDecoder> makeGthDecoder() { return std::make_unique<GthDecoder>(); }

} // namespace dect2
