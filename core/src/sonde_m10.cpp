// Meteomodem M10 (and M10+ with the Gtop GPS): frame bytes <-> fields, the symbol builder and the decoder.
//
// Facts from rs1729/RS demod/mod/m10m20mod.c (frame layout, field scales, serial number, checksum, thermistor, humidity, battery):
//   frame[0] = length (0x64, 0x76 with auxiliary data, 0x66 on newer ones), total bytes = frame[0] + 1, frame[1] = type (0x9F M10 with
//   the Trimble GPS, 0xAF M10+ with the Gtop GPS), checksum over frame[0 .. len-2] stored big endian at frame[len-1 .. len].
//   Trimble packet 0x8F-20 from frame[2]: velocity E, N, U (int16, 0.005 m/s), TOW ms (uint32), latitude and longitude (int32,
//   2^32/360 per degree), height in mm above the ellipsoid (int32), fix flags, number of satellites, GPS-UTC offset, GPS week.
// The serial number example (raw bytes 02 08 3a 31 25 = "310 2 11329", and 02 1a 74 cf 4b = "704 2 23023") is from rs1729/RS issue #7
// and m10/m10_msp430.txt (the sonde's info memory).
// The 32-symbol sync and the bytes 64 9F 20 that follow are in the header comment of m10m20mod.c; dxlAPRS sondeudp.c has the same 0x649F20.
#include "sonde_bits_dm.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2 {
using namespace sondebits;

namespace {

constexpr int kStdLen = 0x64;
constexpr int kPosSN = 0x5D, kPosCnt = 0x62;
constexpr double kB60 = 1073741824.0 / 90.0;        // 2^32 / 360, rs1729 B60B60
constexpr int kLeap = 18;                           // GPS - UTC sent by the generator (the sonde sends its own value)

inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
inline int16_t be16s(const uint8_t* p) { return (int16_t)(((unsigned)p[0] << 8) | p[1]); }
inline void putBe32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
inline void putBe16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
inline int16_t clamp16(double v) { return (int16_t)std::max(-32768.0, std::min(32767.0, std::round(v))); }

// "310-2-11329": year/month digits as hex+decimal, a type digit, then 3 bits and 13 bits
bool parseM10Serial(const std::string& s, uint8_t raw[5]) {
    if (s.size() != 11 || s[3] != '-' || s[5] != '-') return false;
    auto hexv = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1)); };
    auto dec = [](char c) { return c >= '0' && c <= '9' ? c - '0' : -1; };
    const int h = hexv(s[0]), d1 = dec(s[1]), d2 = dec(s[2]), mid = hexv(s[4]), c = dec(s[6]);
    int n = 0;
    for (int i = 7; i < 11; i++) { const int v = dec(s[i]); if (v < 0) return false; n = n * 10 + v; }
    const int dd = d1 * 10 + d2;
    if (h < 0 || d1 < 0 || d2 < 0 || mid < 0 || c < 0 || c > 7 || dd > 15 || n > 8191) return false;
    raw[0] = (uint8_t)mid; raw[1] = 0; raw[2] = (uint8_t)((h << 4) | dd);
    const unsigned v = ((unsigned)c << 13) | (unsigned)n;
    raw[3] = (uint8_t)(v & 0xFF); raw[4] = (uint8_t)(v >> 8);
    return true;
}

void m10SerialFromText(const std::string& s, uint8_t raw[5]) {
    if (parseM10Serial(s, raw)) return;
    uint32_t h = 2166136261u;
    for (char ch : s) { h ^= (uint8_t)ch; h *= 16777619u; }
    raw[0] = 2; raw[1] = 0; raw[2] = (uint8_t)(0x30 | (h & 0xF)); h >>= 4;
    const unsigned v = (1u << 13) | (h & 0x1FFF);
    raw[3] = (uint8_t)(v & 0xFF); raw[4] = (uint8_t)(v >> 8);
}

std::string m10SerialText(const uint8_t raw[5]) {
    char b[24];
    const unsigned n = raw[3] | ((unsigned)raw[4] << 8);
    std::snprintf(b, sizeof b, "%1X%02u-%1X-%1u%04u", (raw[2] >> 4) & 0xF, raw[2] & 0xFu, raw[0] & 0xF, (n >> 13) & 7, n & 0x1FFF);
    return b;
}

// the whole fix of a 0x9F / 0xAF frame whose checksum is right
void parseM10(const uint8_t* f, size_t n, SondeFix& o) {
    const int flen = f[0];
    const bool gtop = f[1] == 0xAF;
    o.type = "M10";
    o.subtype = gtop ? "M10+" : "M10";
    o.frame = -1;
    uint8_t raw[5];
    if (n > (size_t)kPosSN + 5) { std::memcpy(raw, f + kPosSN, 5); o.serial = m10SerialText(raw); }

    if (!gtop && flen >= 0x24 && f[2] == 0x20 && (f[0x1D] & 1) == 0) {
        const double lat = (int32_t)be32(f + 0x0E) / kB60;
        const double lon = (int32_t)be32(f + 0x12) / kB60;
        const double alt = (int32_t)be32(f + 0x16) / 1000.0;
        const int sats = f[0x1E];
        const int ofs = f[0x1F] <= 40 ? f[0x1F] : kLeap;
        const int week = fixTrimbleWeek((f[0x20] << 8) | f[0x21]);
        const uint32_t towMs = be32(f + 0x0A);
        if (std::fabs(lat) <= 90 && std::fabs(lon) <= 180 && !(lat == 0 && lon == 0) && alt > -1000 && alt < 100000) {
            o.hasPos = true; o.lat = lat; o.lon = lon; o.altM = alt;
            o.sats = sats;
            const double vE = be16s(f + 0x04) / 200.0, vN = be16s(f + 0x06) / 200.0, vU = be16s(f + 0x08) / 200.0;
            o.hasVel = true;
            o.hSpeed = std::hypot(vE, vN);
            double hd = std::atan2(vE, vN) * 180.0 / M_PI;
            if (hd < 0) hd += 360.0;
            o.headingDeg = hd; o.vSpeed = vU;
        }
        if (week > 0 && towMs < 604800000u) { o.hasTime = true; o.unixTime = gpsToUnix(week, towMs / 1000.0, ofs); }
    } else if (gtop && flen >= 0x1B) {
        // Gtop packet: lat, lon int32 in 1e-6 degrees, height int24 in 0.01 m, velocities int16 in 0.01 m/s, time and date as decimal digits
        const double lat = (int32_t)be32(f + 0x04) / 1e6, lon = (int32_t)be32(f + 0x08) / 1e6;
        int a = (f[0x0C] << 16) | (f[0x0D] << 8) | f[0x0E];
        if (a & 0x800000) a -= 0x1000000;
        const double alt = a / 100.0;
        if (std::fabs(lat) <= 90 && std::fabs(lon) <= 180 && !(lat == 0 && lon == 0) && alt > -1000 && alt < 100000) {
            o.hasPos = true; o.lat = lat; o.lon = lon; o.altM = alt;
            const double vE = be16s(f + 0x0F) / 100.0, vN = be16s(f + 0x11) / 100.0, vU = be16s(f + 0x13) / 100.0;
            o.hasVel = true; o.hSpeed = std::hypot(vE, vN);
            double hd = std::atan2(vE, vN) * 180.0 / M_PI;
            if (hd < 0) hd += 360.0;
            o.headingDeg = hd; o.vSpeed = vU;
        }
        const int tm = (f[0x15] << 16) | (f[0x16] << 8) | f[0x17], dt = (f[0x18] << 16) | (f[0x19] << 8) | f[0x1A];
        const int hh = tm / 10000, mi = (tm % 10000) / 100, ss = tm % 100, dd = dt / 10000, mo = (dt % 10000) / 100, yy = 2000 + dt % 100;
        if (hh < 24 && mi < 60 && ss < 61 && mo >= 1 && mo <= 12 && dd >= 1 && dd <= 31) {
            o.hasTime = true; o.unixTime = civilToUnix(yy, mo, dd, hh, mi, ss);
        }
    }

    if (flen >= 0x46) {
        // temperature: range byte, then the ADC value plus 0xA000
        const int sc = f[0x3E];
        const int adc = (int)(uint16_t)((f[0x3F] | (f[0x40] << 8)) - 0xA000);
        double t = 0;
        if (m10NtcTemp(sc, adc, t)) {
            o.hasTemp = true; o.tempC = t;
            // humidity: capture count of the sensor oscillator against the 55 % reference count (UPSI sensor), with rs1729's cold correction
            const double ref = (f[0x32] | (f[0x33] << 8) | (f[0x34] << 16)) / 1000.0;
            const double cap = (f[0x35] | (f[0x36] << 8) | (f[0x37] << 16)) / 1000.0;
            if (ref > 0 && cap > 0) {
                double rh = (cap / ref - 0.8955) / 0.002;
                if (t < 0.0) rh += 0.0 - t / 5.5;
                if (t < -30.0) rh *= 1.0 + (-30.0 - t) / 75.0;
                o.hasHumidity = true; o.humidity = std::max(0.0, std::min(100.0, rh));
            }
        }
        const int badc = f[0x45] | (f[0x46] << 8);
        if (badc > 0 && badc < 1100) o.batteryV = 2.709 * badc * 2.5 / 1023.0;
    }
}

class M10Decoder : public DmFrameDecoder {
public:
    M10Decoder() { maxSyncErrors_ = 4; }
    const char* type() const override { return "M10"; }
    double symbolRate() const override { return 9615.0; }      // rs1729: 9614 to 9616 on the M10, 9600 on newer ones; the timing loop follows
protected:
    size_t frameLength(uint8_t b0, uint8_t b1) const override {
        if (b1 != 0x9F && b1 != 0xAF) return 0;                 // 0x49 (satellite levels), 0x8F (M2K2) and the M20 are other frames
        if (b0 < kStdLen || b0 > kStdLen + 64) return 0;
        return (size_t)b0 + 1;
    }
    bool handleFrame(const uint8_t* f, size_t n, double, SondeFix& out) override {
        if (!m10ParseFrame(f, n, out)) {
            out = SondeFix();
            out.type = "M10"; out.crcOk = false;
        }
        return true;
    }
};

} // namespace

uint16_t sondeM10Checksum(const uint8_t* msg, size_t len) { return m10Check(msg, len); }

bool m10ParseFrame(const uint8_t* f, size_t n, SondeFix& out) {
    if (n < 4 || (f[1] != 0x9F && f[1] != 0xAF)) return false;
    const size_t flen = f[0];
    if (flen < (size_t)kStdLen || n < flen + 1) return false;
    const uint16_t cs1 = (uint16_t)((f[flen - 1] << 8) | f[flen]);
    if (cs1 != m10Check(f, flen - 1)) return false;
    out = SondeFix();
    out.crcOk = true;
    parseM10(f, n, out);
    return true;
}

std::vector<uint8_t> m10FrameBytes(const SondeTruth& t) {
    std::vector<uint8_t> f(kStdLen + 1, 0);
    f[0] = kStdLen; f[1] = 0x9F; f[2] = 0x20;
    const double hs = t.hSpeed, hd = t.headingDeg * M_PI / 180.0;
    putBe16(&f[4], (uint16_t)clamp16(hs * std::sin(hd) * 200.0));
    putBe16(&f[6], (uint16_t)clamp16(hs * std::cos(hd) * 200.0));
    putBe16(&f[8], (uint16_t)clamp16(t.vSpeed * 200.0));
    const double unixT = t.unixTime > 0 ? t.unixTime : 1767225600.0;   // 2026-01-01 when the caller has no clock
    int week; double tow;
    unixToGps(unixT, kLeap, week, tow);
    putBe32(&f[0x0A], (uint32_t)std::llround(tow * 1000.0));
    putBe32(&f[0x0E], (uint32_t)(int32_t)std::llround(t.lat * kB60));
    putBe32(&f[0x12], (uint32_t)(int64_t)std::llround(t.lon * kB60));
    putBe32(&f[0x16], (uint32_t)(int32_t)std::llround(t.altM * 1000.0));
    f[0x1C] = 1;                                   // datum WGS-84
    f[0x1D] = 0;                                   // valid 3D fix
    f[0x1E] = (uint8_t)std::max(0, std::min(255, t.sats));
    f[0x1F] = kLeap;
    putBe16(&f[0x20], (uint32_t)week);
    // sensors
    // humidity: counts of 1000 oscillator edges; the reference is the 55 % count, the ratio carries the humidity
    double tc = t.tempC;
    int sc = 0, adc = 0;
    if (m10NtcAdc(tc, sc, adc)) {
        f[0x3E] = (uint8_t)sc;
        const unsigned raw = (unsigned)(adc + 0xA000);
        f[0x3F] = (uint8_t)(raw & 0xFF); f[0x40] = (uint8_t)(raw >> 8);
        double rh = t.humidity;
        // undo the cold correction of the decoder (rh_final = (rh_raw - t/5.5 [t<0]) * (1 + (-30 - t)/75 [t<-30]))
        if (tc < -30.0) rh /= 1.0 + (-30.0 - tc) / 75.0;
        if (tc < 0.0) rh -= 0.0 - tc / 5.5;
        const double ratio = 0.8955 + 0.002 * rh;
        const uint32_t ref = 100000, cap = (uint32_t)std::llround(ref * ratio);
        f[0x32] = ref & 0xFF; f[0x33] = (ref >> 8) & 0xFF; f[0x34] = (ref >> 16) & 0xFF;
        f[0x35] = cap & 0xFF; f[0x36] = (cap >> 8) & 0xFF; f[0x37] = (cap >> 16) & 0xFF;
    }
    const unsigned badc = (unsigned)std::llround(t.batteryV * 1023.0 / (2.709 * 2.5));
    f[0x45] = badc & 0xFF; f[0x46] = (badc >> 8) & 0xFF;
    uint8_t raw[5];
    m10SerialFromText(t.serial, raw);
    std::memcpy(&f[kPosSN], raw, 5);
    f[kPosCnt] = (uint8_t)(t.frame & 0xFF);
    const uint16_t cs = m10Check(f.data(), kStdLen - 1);
    f[kStdLen - 1] = (uint8_t)(cs >> 8); f[kStdLen] = (uint8_t)cs;
    return f;
}

std::unique_ptr<SondeBitDecoder> makeM10Decoder() { return std::unique_ptr<SondeBitDecoder>(new M10Decoder()); }

std::vector<uint8_t> m10Symbols(const SondeTruth& t) {
    std::vector<uint8_t> sym;
    DmWriter w(sym);
    for (int i = 0; i < 32; i++) w.bit(0);          // preamble: the symbol clock
    w.sync();
    for (uint8_t b : m10FrameBytes(t)) w.byte(b);
    return sym;
}

} // namespace dect2
