// Meteomodem M20: frame bytes <-> fields, the symbol builder and the decoder. Same line code and checksum as the M10, a different frame.
//
// Facts from rs1729/RS demod/mod/m10m20mod.c: frame[0] = length (0x45, sometimes 0x43), total bytes = frame[0] + 1, frame[1] = 0x20,
// checksum (the M10 checksum) over frame[0 .. len-2] at frame[len-1 .. len], big endian. The most important data come first:
//   0x02 humidity ADC, 0x04 temperature ADC (+4096 per range step), 0x06 humidity sensor temperature ADC (all little endian),
//   0x08 height (3 bytes, 0.01 m, signed), 0x0B velocity E and 0x0D N (int16, 0.01 m/s), 0x0F time of week (3 bytes, seconds),
//   0x12 serial number (3 bytes), 0x15 counter, 0x16 block checksum (firmware < 7), 0x18 velocity U, 0x1A GPS week,
//   0x1C latitude and 0x20 longitude (int32, 1e-6 degree), 0x26 battery (8-bit ADC, 3.3 V), 0x43 firmware version.
// The same positions, scales and the 18 s are in oe5hpm/dxlAPRS src/sondeudp.c and dl9rdz/rdz_ttgo_sonde RX_FSK/src/M10M20.cpp
// (independent checks of every offset above, of the 18 s, of the battery byte and of the serial number layout).
// Sync and symbol rate (9600) as the M10: header comment of m10m20mod.c; the bytes after the sync are 45 20.
// Left out because the sources are not clear: humidity (rs1729 marks the transfer function with a question mark), pressure.
#include "sonde_bits_dm.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dect2 {
using namespace sondebits;

namespace {

constexpr int kStdLen = 0x45;
constexpr int kLeap = 18;                       // GPS - UTC: dxlAPRS sondeudp.c subtracts 18 s from the M20 time of week

inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
inline int16_t be16s(const uint8_t* p) { return (int16_t)(((unsigned)p[0] << 8) | p[1]); }
inline void putBe32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
inline void putBe16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
inline void putBe24(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }
inline int16_t clamp16(double v) { return (int16_t)std::max(-32768.0, std::min(32767.0, std::round(v))); }

// "211-4-01234": year digit, month (2 digits), a digit 1..8, bit 0 or 1, 13-bit number
bool parseM20Serial(const std::string& s, uint32_t& sn24) {
    if (s.size() != 11 || s[3] != '-' || s[5] != '-') return false;
    auto dec = [](char c) { return c >= '0' && c <= '9' ? c - '0' : -1; };
    const int y = dec(s[0]), m = dec(s[1]) * 10 + dec(s[2]), d = dec(s[4]), b = dec(s[6]);
    int n = 0;
    for (int i = 7; i < 11; i++) { const int v = dec(s[i]); if (v < 0) return false; n = n * 10 + v; }
    if (y < 0 || dec(s[1]) < 0 || dec(s[2]) < 0 || m < 1 || m > 12 || d < 1 || d > 8 || b < 0 || b > 1 || n > 8191) return false;
    sn24 = (uint32_t)(y * 12 + (m - 1)) | ((uint32_t)(d - 1) << 7) | ((uint32_t)n << 10) | ((uint32_t)b << 23);
    return true;
}

uint32_t m20SerialFromText(const std::string& s) {
    uint32_t v;
    if (parseM20Serial(s, v)) return v;
    uint32_t h = 2166136261u;
    for (char ch : s) { h ^= (uint8_t)ch; h *= 16777619u; }
    return (uint32_t)(((h >> 3) % 120)) | ((uint32_t)((h >> 10) & 7) << 7) | ((uint32_t)((h >> 13) & 0x1FFF) << 10);
}

std::string m20SerialText(uint32_t sn24) {
    const unsigned ym = sn24 & 0x7F;
    char b[24];
    std::snprintf(b, sizeof b, "%u%02u-%u-%u%04u", ym / 12, ym % 12 + 1, ((sn24 >> 7) & 7) + 1, (sn24 >> 23) & 1, (sn24 >> 10) & 0x1FFF);
    return b;
}

class M20Decoder : public DmFrameDecoder {
public:
    M20Decoder() { maxSyncErrors_ = 4; }
    const char* type() const override { return "M20"; }
    double symbolRate() const override { return 9600.0; }
protected:
    size_t frameLength(uint8_t b0, uint8_t b1) const override {
        if (b1 != 0x20) return 0;
        if (b0 < 0x43 || b0 > 0x6F) return 0;
        return (size_t)b0 + 1;
    }
    bool handleFrame(const uint8_t* f, size_t n, double, SondeFix& out) override {
        if (!m20ParseFrame(f, n, out)) {
            out = SondeFix();
            out.type = "M20"; out.crcOk = false;
        }
        return true;
    }
};

} // namespace

bool m20ParseFrame(const uint8_t* f, size_t n, SondeFix& o) {
    if (n < 4 || f[1] != 0x20) return false;
    const size_t flen = f[0];
    if (flen < 0x43 || n < flen + 1) return false;
    const uint16_t cs1 = (uint16_t)((f[flen - 1] << 8) | f[flen]);
    if (cs1 != m10Check(f, flen - 1)) return false;
    o = SondeFix();
    o.type = "M20"; o.subtype = "M20"; o.crcOk = true; o.frame = -1;
    if (flen < 0x25) return true;
    o.serial = m20SerialText((uint32_t)f[0x14] << 16 | (uint32_t)f[0x13] << 8 | f[0x12]);

    const double lat = (int32_t)be32(f + 0x1C) / 1e6, lon = (int32_t)be32(f + 0x20) / 1e6;
    int alt24 = (f[0x08] << 16) | (f[0x09] << 8) | f[0x0A];
    if (alt24 & 0x800000) alt24 -= 0x1000000;                  // signed: dxlAPRS and rdz_ttgo_sonde read it so
    const double alt = alt24 / 100.0;
    if (std::fabs(lat) <= 90 && std::fabs(lon) <= 180 && !(lat == 0 && lon == 0) && alt < 100000 && alt > -1000) {
        o.hasPos = true; o.lat = lat; o.lon = lon; o.altM = alt;
        const double vE = be16s(f + 0x0B) / 100.0, vN = be16s(f + 0x0D) / 100.0, vU = be16s(f + 0x18) / 100.0;
        o.hasVel = true; o.hSpeed = std::hypot(vE, vN);
        double hd = std::atan2(vE, vN) * 180.0 / M_PI;
        if (hd < 0) hd += 360.0;
        o.headingDeg = hd; o.vSpeed = vU;
        const int week = fixTrimbleWeek((f[0x1A] << 8) | f[0x1B]);
        const uint32_t tow = ((uint32_t)f[0x0F] << 16) | (f[0x10] << 8) | f[0x11];
        if (week > 0 && tow < 604800u) { o.hasTime = true; o.unixTime = gpsToUnix(week, tow, kLeap); }
    }
    // temperature: the ADC value carries the range in bits 12 and 13 (rs1729)
    int adc = f[0x04] | (f[0x05] << 8), sc = 0;
    if (adc > 8191) { sc = 2; adc -= 8192; } else if (adc > 4095) { sc = 1; adc -= 4096; }
    double t;
    if (m10NtcTemp(sc, adc, t)) { o.hasTemp = true; o.tempC = t; }
    if (f[0x26] > 0) o.batteryV = f[0x26] * (3.3 / 255.0);
    return true;
}

std::vector<uint8_t> m20FrameBytes(const SondeTruth& t) {
    std::vector<uint8_t> f(kStdLen + 1, 0);
    f[0] = kStdLen; f[1] = 0x20;
    int sc = 0, adc = 0;
    if (m10NtcAdc(t.tempC, sc, adc)) { const unsigned v = (unsigned)(adc + 4096 * sc); f[4] = v & 0xFF; f[5] = v >> 8; }
    f[2] = 0x20; f[3] = 0x4E;                       // humidity ADC (not decoded): a plausible value
    f[6] = 0xD0; f[7] = 0x07;                       // humidity sensor temperature ADC (not decoded)
    putBe24(&f[0x08], (uint32_t)((int32_t)std::max(-8388608.0, std::min(8388607.0, std::round(t.altM * 100.0))) & 0xFFFFFF));
    const double hs = t.hSpeed, hd = t.headingDeg * M_PI / 180.0;
    putBe16(&f[0x0B], (uint16_t)clamp16(hs * std::sin(hd) * 100.0));
    putBe16(&f[0x0D], (uint16_t)clamp16(hs * std::cos(hd) * 100.0));
    const double unixT = t.unixTime > 0 ? t.unixTime : 1767225600.0;
    int week; double tow;
    unixToGps(unixT, kLeap, week, tow);
    putBe24(&f[0x0F], (uint32_t)std::llround(tow));
    const uint32_t sn = m20SerialFromText(t.serial);
    f[0x12] = sn & 0xFF; f[0x13] = (sn >> 8) & 0xFF; f[0x14] = (sn >> 16) & 0xFF;
    f[0x15] = (uint8_t)(t.frame & 0xFF);
    putBe16(&f[0x18], (uint16_t)clamp16(t.vSpeed * 100.0));
    putBe16(&f[0x1A], (uint32_t)week);
    putBe32(&f[0x1C], (uint32_t)(int32_t)std::llround(t.lat * 1e6));
    putBe32(&f[0x20], (uint32_t)(int32_t)std::llround(t.lon * 1e6));
    f[0x26] = (uint8_t)std::max(0.0, std::min(255.0, std::round(t.batteryV * 255.0 / 3.3)));
    f[0x43] = 0x06;                                 // firmware below 7: the block checksum sits at 0x16
    const uint16_t bc = m10BlockCheck(0x16, f.data() + 2);
    f[0x16] = (uint8_t)(bc >> 8); f[0x17] = (uint8_t)bc;
    const uint16_t cs = m10Check(f.data(), kStdLen - 1);
    f[kStdLen - 1] = (uint8_t)(cs >> 8); f[kStdLen] = (uint8_t)cs;
    return f;
}

std::unique_ptr<SondeBitDecoder> makeM20Decoder() { return std::unique_ptr<SondeBitDecoder>(new M20Decoder()); }

std::vector<uint8_t> m20Symbols(const SondeTruth& t) {
    std::vector<uint8_t> sym;
    DmWriter w(sym);
    for (int i = 0; i < 32; i++) w.bit(0);
    w.sync();
    for (uint8_t b : m20FrameBytes(t)) w.byte(b);
    return sym;
}

} // namespace dect2
