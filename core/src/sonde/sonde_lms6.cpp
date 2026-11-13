// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Lockheed Martin LMS6 at 1680 MHz (Sippican MK2A telemetry): UART 8N1 at 9616 baud, CRC-16, GPS subframe 0x245254.
// Facts from rs1729/RS mk2a/mk2a_lms1680.c and mk2a.txt (field offsets, scales, checksum); implementation is our own.
#include "dect2/sonde/sonde_bits.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2 {

namespace {

constexpr double kBaud = 9616.0;
constexpr double kB60 = 11930464.0 / 90.0;   // 2^32 / 360
constexpr int kOfs = 2;                       // frame[2] = subframe type (0x54 GPS, 0x4D met/config)
constexpr int kPosId = kOfs + 2;
constexpr int kPosFrame = kOfs + 4;
constexpr int kPosTow = kOfs + 8;
constexpr int kPosLat = kOfs + 0x10;
constexpr int kPosLon = kOfs + 0x14;
constexpr int kPosAlt = kOfs + 0x18;
constexpr int kPosVe = kOfs + 0x1C;
constexpr int kPosVn = kOfs + 0x1F;
constexpr int kPosVu = kOfs + 0x22;
constexpr int kPosFullId = kOfs + 0x30;

uint16_t lms6Crc16(const uint8_t* frame, int len) {
    int rem = 0;
    for (int i = 0; i < len; i++) {
        rem ^= frame[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (rem & 0x8000) rem = ((rem << 1) ^ 0x1021) & 0xFFFF;
            else rem = (rem << 1) & 0xFFFF;
        }
    }
    return (uint16_t)rem;
}

inline int32_t be32s(const uint8_t* p) {
    return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]);
}

inline int32_t be24s(const uint8_t* p) {
    int v = (p[0] << 16) | (p[1] << 8) | p[2];
    if (v & 0x800000) v -= 0x1000000;
    return v;
}

inline void putBe16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
inline void putBe32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
inline void putBe24(uint8_t* p, int32_t v) {
    p[0] = (uint8_t)((v >> 16) & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF); p[2] = (uint8_t)(v & 0xFF);
}

uint32_t idFromSerial(const std::string& s) {
    if (s.empty()) return 0x12345678u;
    size_t p = s.find('-');
    const char* t = p == std::string::npos ? s.c_str() : s.c_str() + p + 1;
    char* end = nullptr;
    const unsigned long v = std::strtoul(t, &end, 10);
    if (end && end > t) return (uint32_t)v;
    uint32_t h = 2166136261u;
    for (char ch : s) { h ^= (uint8_t)ch; h *= 16777619u; }
    return h;
}

void uartBits(std::vector<uint8_t>& out, uint8_t b) {
    out.push_back(0);
    for (int i = 0; i < 8; i++) out.push_back((uint8_t)((b >> i) & 1));
    out.push_back(1);
}

bool parseGpsSubframe(const uint8_t* f, size_t n, SondeFix& o, uint32_t& fullId) {
    if (n < (size_t)kPosVu + 6 || f[0] != 0x24 || f[kOfs] != 0x54) return false;
    size_t flen = n;
    while (flen > 2 && f[flen - 1] == 0xCA) flen--;
    if (flen < 40) return false;
    int crclen = (int)flen - 2;
    const uint16_t got = (uint16_t)((f[crclen] << 8) | f[crclen + 1]);
    if (got != lms6Crc16(f, crclen)) {
        if (got != lms6Crc16(f, crclen + 1) && got != lms6Crc16(f, (int)flen)) return false;
    }
    o.type = "LMS6";
    o.subtype = "LMS6-1680";
    o.crcOk = true;
    o.frame = (f[kPosFrame] << 8) | f[kPosFrame + 1];
    const uint32_t sid = ((uint32_t)f[kPosId] << 8) | f[kPosId + 1];
    if (fullId == 0 || (fullId & 0xFFFF) != sid) fullId = sid;
    char ser[16];
    std::snprintf(ser, sizeof ser, "%u", (unsigned)(fullId & 0xFFFFFFFFu));
    o.serial = ser;

    const double lat = be32s(f + kPosLat) / kB60;
    const double lon = be32s(f + kPosLon) / kB60;
    const double alt = be32s(f + kPosAlt) / 1000.0;
    if (std::fabs(lat) <= 90 && std::fabs(lon) <= 180 && !(lat == 0 && lon == 0) && alt > -500 && alt < 65000) {
        o.hasPos = true;
        o.lat = lat;
        o.lon = lon;
        o.altM = alt;
    }
    const double vE = be24s(f + kPosVe) / 1000.0, vN = be24s(f + kPosVn) / 1000.0, vU = be24s(f + kPosVu) / 1000.0;
    o.hasVel = true;
    o.vSpeed = vU;
    o.hSpeed = std::hypot(vE, vN);
    double hd = std::atan2(vE, vN) * 180.0 / M_PI;
    if (hd < 0) hd += 360.0;
    o.headingDeg = hd;

    const uint32_t towMs = (uint32_t)be32s(f + kPosTow);
    const int day = (int)(towMs / 86400000u);
    const uint32_t msDay = towMs % 86400000u;
    const int sec = (int)(msDay / 1000u);
    const int hh = sec / 3600, mi = (sec % 3600) / 60, ss = sec % 60;
    if (day >= 0 && day <= 6 && hh < 24 && mi < 60) {
        o.hasTime = true;
        // GPS-time-of-week within the week (no calendar date on this subframe)
        o.unixTime = day * 86400.0 + hh * 3600.0 + mi * 60.0 + ss;
    }
    return true;
}

bool parseMetSubframe(const uint8_t* f, size_t n, uint32_t& fullId) {
    if (n < (size_t)kPosFullId + 4 || f[0] != 0x24 || f[kOfs] != 0x4D) return false;
    size_t flen = n;
    while (flen > 2 && f[flen - 1] == 0xCA) flen--;
    if (flen < 12) return false;
    const int crclen = (int)flen - 2;
    const uint16_t got = (uint16_t)((f[crclen] << 8) | f[crclen + 1]);
    if (got != lms6Crc16(f, crclen)) return false;
    if (f[kPosId] == f[kPosFullId] && f[kPosId + 1] == f[kPosFullId + 1]) {
        fullId = ((uint32_t)f[kPosFullId + 2] << 24) | ((uint32_t)f[kPosFullId + 3] << 16) | ((uint32_t)f[kPosId] << 8) | f[kPosId + 1];
    }
    return true;
}

class Lms6Decoder : public SondeBitDecoder {
public:
    const char* type() const override { return "LMS6"; }
    double symbolRate() const override { return kBaud; }
    void reset() override {
        bitInByte_ = 0;
        bitIdx_ = 0;
        stream_.clear();
        fullId_ = 0;
    }
    void push(const uint8_t* sym, size_t n, double, std::vector<SondeFix>& out) override {
        for (size_t i = 0; i < n; i++) pushUart(sym[i], out);
    }

private:
    int bitIdx_ = 0;
    uint16_t bitInByte_ = 0;
    std::vector<uint8_t> stream_;
    uint32_t fullId_ = 0;

    void pushUart(uint8_t s, std::vector<SondeFix>& out) {
        if (bitIdx_ == 0) {
            if (s != 0) return;
            bitIdx_ = 1;
            bitInByte_ = 0;
            return;
        }
        if (bitIdx_ >= 1 && bitIdx_ <= 8) {
            if (s) bitInByte_ |= (uint16_t)(1u << (bitIdx_ - 1));
            bitIdx_++;
            return;
        }
        if (bitIdx_ == 9) {
            const bool ok = s == 1;
            bitIdx_ = 0;
            if (!ok) return;
            onByte((uint8_t)bitInByte_, out);
        }
    }

    void onByte(uint8_t b, std::vector<SondeFix>& out) {
        stream_.push_back(b);
        if (stream_.size() > 512) stream_.erase(stream_.begin(), stream_.begin() + (long)stream_.size() - 400);
        for (size_t i = 0; i + 8 < stream_.size(); i++) {
            if (stream_[i] != 0x24) continue;
            if (stream_[i + kOfs] == 0x4D) parseMetSubframe(stream_.data() + i, stream_.size() - i, fullId_);
            if (stream_[i + kOfs] != 0x54) continue;
            SondeFix fx;
            if (!parseGpsSubframe(stream_.data() + i, stream_.size() - i, fx, fullId_)) continue;
            out.push_back(fx);
            stream_.erase(stream_.begin(), stream_.begin() + (long)i + 1);
            return;
        }
    }
};

} // namespace

uint16_t sondeLms6Crc16(const uint8_t* msg, size_t len) { return lms6Crc16(msg, (int)len); }

std::vector<uint8_t> lms6GpsFrameBytes(const SondeTruth& t) {
    std::vector<uint8_t> f(180, 0);
    f[0] = 0x24;
    f[1] = 0x52;
    f[kOfs] = 0x54;
    const uint32_t id = idFromSerial(t.serial);
    f[kPosId] = (uint8_t)(id >> 8);
    f[kPosId + 1] = (uint8_t)id;
    putBe16(f.data() + kPosFrame, (uint16_t)(t.frame & 0xFFFF));
    const int day = 3;
    const int secInWeek = day * 86400 + ((int)std::fmod(t.unixTime, 86400.0));
    const uint32_t towMs = (uint32_t)(secInWeek * 1000 + (int)std::fmod(t.unixTime * 1000.0, 1000.0));
    putBe32(f.data() + kPosTow, towMs);
    putBe32(f.data() + kPosLat, (uint32_t)(int32_t)std::llround(t.lat * kB60));
    putBe32(f.data() + kPosLon, (uint32_t)(int32_t)std::llround(t.lon * kB60));
    putBe32(f.data() + kPosAlt, (uint32_t)(int32_t)std::llround(t.altM * 1000.0));
    const double hd = t.headingDeg * M_PI / 180.0;
    putBe24(f.data() + kPosVe, (int32_t)std::llround(t.hSpeed * std::sin(hd) * 1000.0));
    putBe24(f.data() + kPosVn, (int32_t)std::llround(t.hSpeed * std::cos(hd) * 1000.0));
    putBe24(f.data() + kPosVu, (int32_t)std::llround(t.vSpeed * 1000.0));
    const size_t body = 174;
    const uint16_t cs = lms6Crc16(f.data(), (int)body);
    f[body] = (uint8_t)(cs >> 8);
    f[body + 1] = (uint8_t)cs;
    f.resize(body + 2);
    return f;
}

std::vector<uint8_t> lms6UartPreamble() {
    std::vector<uint8_t> s;
    for (int i = 0; i < 4; i++) uartBits(s, 0xCA);
    uartBits(s, 0x24);
    return s;
}

std::vector<uint8_t> lms6Symbols(const SondeTruth& t) {
    std::vector<uint8_t> s = lms6UartPreamble();
    for (uint8_t b : lms6GpsFrameBytes(t)) uartBits(s, b);
    return s;
}

bool lms6ParseFrame(const uint8_t* f, size_t n, SondeFix& out) {
    uint32_t id = 0;
    if (!parseGpsSubframe(f, n, out, id)) return false;
    return out.crcOk;
}

std::unique_ptr<SondeBitDecoder> makeLms6Decoder() { return std::make_unique<Lms6Decoder>(); }

} // namespace dect2
