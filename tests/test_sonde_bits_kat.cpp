// Known-answer tests of the radiosonde bit decoders (DFM, M10, M20). Every expected value comes from a published source, named at the check:
//  [rs]  rs1729/RS demod/mod/dfm09mod.c, m10m20mod.c (header comment with the sync symbols), issue #7 and m10/m10_msp430.txt (serial numbers)
//  [dxl] oe5hpm/dxlAPRS src/sondeudp.c: a second implementation of the checksum, the byte offsets and scales, the Hamming table
// No real recording is used (none was available); a frame that came from a real sonde is the one thing these tests do not contain.
#include "data/sonde/bits/util.h"
#include "../core/src/sonde_bits_dm.h"
#include <cstring>
using namespace sbt;
using namespace dect2::sondebits;

// [dxl] sondeudp.c crcm10(): the same checksum written as bit operations on a 16-bit state
static uint16_t dxlCrcM10(const uint8_t* buf, int len) {
    uint16_t cs = 0;
    for (int i = 0; i < len; i++) {
        uint16_t b = buf[i];
        b = (uint16_t)((b >> 1) | ((b & 1u) << 7));
        b = (uint16_t)(b ^ ((b >> 2) & 0xFFu));
        const uint16_t t = (uint16_t)((cs & 0x3Fu) | ((((cs ^ (cs >> 2) ^ (cs >> 4)) & 1u)) << 6) | ((((cs >> 1) ^ (cs >> 3) ^ (cs >> 5)) & 1u) << 7));
        uint16_t s = (uint16_t)((cs >> 7) & 0xFFu);
        s = (uint16_t)((s ^ (s >> 2)) & 0xFFu);
        cs = (uint16_t)(((cs & 0xFFu) << 8) | (b ^ t ^ s));
    }
    return cs;
}

static uint32_t dxlCard(const std::vector<uint8_t>& b, int pos, int len) {   // [dxl] m10card: big endian
    uint32_t n = 0;
    for (int i = 0; i < len; i++) n = n * 256 + b[(size_t)(pos + i)];
    return n;
}

static SondeTruth truth() {
    SondeTruth t;
    t.lat = 25.2048; t.lon = 55.2708; t.altM = 12345.6; t.vSpeed = 5.0; t.hSpeed = 10.0; t.headingDeg = 90.0;
    t.sats = 9; t.unixTime = 1767268800.0 + 17.0;     // 2026-01-01 12:00:17 UTC
    t.tempC = -48.3; t.humidity = 31.0; t.batteryV = 4.8;
    return t;
}

int main() {
    // ---- Hamming(8,4): the table printed in the comment of [dxl] hamming(): data nibble, then the 4 parity bits ----
    {
        static const char* par[16] = {"0000", "1110", "1101", "0011", "1011", "0101", "0110", "1000", "0111", "1001", "1010", "0100", "1100", "0010", "0001", "1111"};
        for (int n = 0; n < 16; n++) {
            const uint8_t cw = sondeHamming84Encode((uint8_t)n);
            unsigned p = 0;
            for (int i = 0; i < 4; i++) p = (p << 1) | (unsigned)(par[n][i] - '0');
            CHECK((cw >> 4) == n && (cw & 0xF) == p, "[dxl] Hamming table: nibble %X -> %02X, expected parity %s", n, cw, par[n]);
            uint8_t nib;
            CHECK(sondeHamming84Decode(cw, nib) == 0 && nib == n, "clean codeword %02X", cw);
            for (int b = 0; b < 8; b++) {      // [rs] dfm09mod.c check(): one error is corrected
                CHECK(sondeHamming84Decode((uint8_t)(cw ^ (1 << b)), nib) == 1 && nib == n, "one error, nibble %X bit %d", n, b);
            }
            for (int b = 0; b < 8; b++) for (int c = b + 1; c < 8; c++) {     // two errors are detected, never mistaken for one
                CHECK(sondeHamming84Decode((uint8_t)(cw ^ (1 << b) ^ (1 << c)), nib) == -1, "two errors, nibble %X bits %d %d", n, b, c);
            }
        }
    }
    // ---- M10 checksum: our code against the second implementation, on random messages and on 0x00 / 0xFF ----
    {
        Rng r(11);
        for (int k = 0; k < 3000; k++) {
            uint8_t m[120];
            const int len = r.range(1, 120);
            for (int i = 0; i < len; i++) m[i] = (uint8_t)r.next();
            CHECK(sondeM10Checksum(m, (size_t)len) == dxlCrcM10(m, len), "[dxl] checksum, message %d (len %d)", k, len);
        }
        uint8_t z[99] = {0}, f[99];
        std::memset(f, 0xFF, sizeof f);
        CHECK(sondeM10Checksum(z, 99) == 0, "checksum of zeros is zero (linear code)");
        CHECK(sondeM10Checksum(f, 99) == dxlCrcM10(f, 99), "[dxl] checksum of 0xFF");
    }
    // ---- sync ----
    {
        // [rs] m10m20mod.c header comment: 5 rows of 16 symbols; rows 1-2 are the sync, rows 3-5 are the bits 0110 0100, 1001 1111, 0010 0000 = 64 9F 20
        const char* rows[5] = {"1100110011001100", "1010011001001100", "1101010011010011", "0100110101010101", "0011010011001100"};
        CHECK(std::strcmp(kDmSync, "11001100110011001010011001001100") == 0, "sync symbols are rows 1 and 2");
        std::vector<uint8_t> sym;
        DmWriter w(sym);
        w.sync();
        w.byte(0x64); w.byte(0x9F); w.byte(0x20);
        std::string s;
        for (uint8_t x : sym) s += (char)('0' + x);
        std::string expect;
        for (int i = 0; i < 5; i++) expect += rows[i];
        CHECK(s == expect, "[rs] 64 9F 20 after the sync gives the 80 symbols of the header comment:\n  %s\n  %s", s.c_str(), expect.c_str());
        // [dxl] sondeudp.c: the M10 sync word is 0x649F20
        const SondeTruth t = truth();
        const auto fr = m10FrameBytes(t);
        CHECK(((uint32_t)fr[0] << 16 | (uint32_t)fr[1] << 8 | fr[2]) == 0x649F20u, "[dxl] M10 frame starts with 64 9F 20");
        CHECK(fr.size() == 101, "M10 frame is 0x64 + 1 bytes");
        const auto m2 = m20FrameBytes(t);
        CHECK(m2.size() == 70 && m2[0] == 0x45 && m2[1] == 0x20, "M20 frame starts with 45 20 [rs]");
        // [rs] dfm09mod.c: raw header 10011010100110010101101001010101 is Manchester for 0x45CF (pair 10 = 0, pair 01 = 1)
        const char* raw = "10011010100110010101101001010101";
        unsigned h = 0;
        for (int i = 0; i < 16; i++) h = (h << 1) | (unsigned)(raw[2 * i + 1] - '0');
        CHECK(h == 0x45CF, "[rs] DFM raw header decodes to 0x45CF (%04X)", h);
        const auto d9 = dfmSymbols(t, 9), d6 = dfmSymbols(t, 6);
        CHECK(d9.size() == 560 && d6.size() == 560, "DFM frame = 560 symbols");
        std::string a, b;
        for (int i = 0; i < 32; i++) { a += (char)('0' + d9[(size_t)i]); b += (char)('0' + d6[(size_t)i]); }
        CHECK(a == raw, "DFM-09 symbols start with the raw header: %s", a.c_str());
        std::string inv = raw;
        for (auto& c : inv) c = c == '0' ? '1' : '0';
        CHECK(b == inv, "DFM-06 uses the opposite Manchester pairing: %s", b.c_str());
    }
    // ---- M10 serial numbers: raw bytes and what the sonde has printed on it ----
    {
        // [rs] issue #7 and m10/m10_msp430.txt: raw 02 08 3a 31 25 = "310 2 11329" (year/month 2013-10), 02 1a 74 cf 4b = "704 2 23023" (Gtop)
        struct { uint8_t raw[5]; const char* sn; } cases[2] = {{{0x02, 0x08, 0x3a, 0x31, 0x25}, "310-2-11329"}, {{0x02, 0x1a, 0x74, 0xcf, 0x4b}, "704-2-23023"}};
        for (const auto& c : cases) {
            auto fr = m10FrameBytes(truth());
            std::memcpy(&fr[0x5D], c.raw, 5);
            const uint16_t cs = sondeM10Checksum(fr.data(), 99);
            fr[99] = (uint8_t)(cs >> 8); fr[100] = (uint8_t)cs;
            SondeFix fx;
            CHECK(m10ParseFrame(fr.data(), fr.size(), fx), "frame with serial %s parses", c.sn);
            CHECK(fx.serial == c.sn, "[rs] serial of raw %02x %02x %02x %02x %02x: got %s, expected %s", c.raw[0], c.raw[1], c.raw[2], c.raw[3], c.raw[4], fx.serial.c_str(), c.sn);
            // and the other direction: the printed number makes the same bytes (the second byte is not part of the number)
            SondeTruth t = truth(); t.serial = c.sn;
            const auto g = m10FrameBytes(t);
            CHECK(g[0x5D] == c.raw[0] && g[0x5F] == c.raw[2] && g[0x60] == c.raw[3] && g[0x61] == c.raw[4], "serial %s -> bytes", c.sn);
        }
    }
    // ---- byte offsets, scales and the time formula against the second implementation [dxl] ----
    {
        const SondeTruth t = truth();
        const auto fr = m10FrameBytes(t);
        // [dxl] M10: tow ms at 10, lat at 14, lon at 18 (x 360/2^32), alt mm at 22, velocities at 4/6/8 (x 0.005), GPS-UTC at 31, week at 32
        const uint32_t tow = dxlCard(fr, 10, 4), week = dxlCard(fr, 32, 2), corr = fr[31];
        const double unixDxl = (double)(tow / 1000u + week * 604800u + 315964800u) - corr;
        CHECK(std::fabs(unixDxl - t.unixTime) < 0.5, "[dxl] time formula: %.1f vs %.1f", unixDxl, t.unixTime);
        const double lat = (double)(int32_t)dxlCard(fr, 14, 4) * 8.3819036711397E-8, lon = (double)(int32_t)dxlCard(fr, 18, 4) * 8.3819036711397E-8;
        // (dxlAPRS writes the scale as a rounded decimal: 3e-6 degree at 55 E)
        CHECK(std::fabs(lat - t.lat) < 1e-5 && std::fabs(lon - t.lon) < 1e-5, "[dxl] lat/lon %.6f %.6f", lat, lon);
        CHECK(std::fabs((double)dxlCard(fr, 22, 4) * 0.001 - t.altM) < 0.002, "[dxl] alt");
        int ci = (int)dxlCard(fr, 4, 2); if (ci > 32767) ci -= 65536;
        const double ve = ci * 0.005;
        ci = (int)dxlCard(fr, 6, 2); if (ci > 32767) ci -= 65536;
        const double vn = ci * 0.005;
        ci = (int)dxlCard(fr, 8, 2); if (ci > 32767) ci -= 65536;
        CHECK(std::fabs(ve - 10.0) < 0.01 && std::fabs(vn) < 0.01 && std::fabs(ci * 0.005 - 5.0) < 0.01, "[dxl] velocity E %.3f N %.3f U %.3f", ve, vn, ci * 0.005);
        CHECK(dxlCard(fr, 99, 2) == dxlCrcM10(fr.data(), 99), "[dxl] checksum position: bytes 99 and 100, over 99 bytes");

        // [dxl] M20: tow (3 bytes) at 15, week at 26, lat/lon at 28/32 x 1e-6, alt (3 bytes) at 8 x 0.01, velocity E/N at 11/13 x 0.01, U at 24; time = tow + week*604800 + 315964800 - 18
        const auto m = m20FrameBytes(t);
        const uint32_t tow2 = dxlCard(m, 15, 3), week2 = dxlCard(m, 26, 2);
        const double unix2 = (double)(tow2 + week2 * 604800u + 315964800u) - 18.0;
        CHECK(std::fabs(unix2 - t.unixTime) < 0.5, "[dxl] M20 time formula: %.1f vs %.1f", unix2, t.unixTime);
        CHECK(std::fabs((double)(int32_t)dxlCard(m, 28, 4) * 1e-6 - t.lat) < 1e-6 && std::fabs((double)(int32_t)dxlCard(m, 32, 4) * 1e-6 - t.lon) < 1e-6, "[dxl] M20 lat/lon");
        CHECK(std::fabs((double)dxlCard(m, 8, 3) * 0.01 - t.altM) < 0.01, "[dxl] M20 alt");
        ci = (int)dxlCard(m, 11, 2); if (ci > 32767) ci -= 65536;
        CHECK(std::fabs(ci * 0.01 - 10.0) < 0.01, "[dxl] M20 velocity E");
        ci = (int)dxlCard(m, 24, 2); if (ci > 32767) ci -= 65536;
        CHECK(std::fabs(ci * 0.01 - 5.0) < 0.01, "[dxl] M20 velocity U");
        CHECK(dxlCard(m, 68, 2) == dxlCrcM10(m.data(), 68), "[dxl] M20 checksum: bytes 68 and 69, over 68 bytes");
        CHECK(dxlCard(m, 22, 2) == m10BlockCheck(0x16, m.data() + 2), "[rs] M20 block checksum: length byte 0x16 then bytes 2..21");
    }
    // ---- DFM time conversions: a date and its seconds since 1970 ----
    {
        // 2026-01-01 00:00:00 UTC = 1767225600 (days from civil: 20454)
        CHECK(civilToUnix(2026, 1, 1, 0, 0, 0) == 1767225600.0, "civilToUnix 2026-01-01");
        CHECK(civilToUnix(1980, 1, 6, 0, 0, 0) == kGpsEpochUnix, "GPS epoch is 1980-01-06");
        int w; double s;
        unixToGps(1767225600.0, 18, w, s);
        CHECK(w == 2399 && std::fabs(s - 345618.0) < 1e-6, "GPS week of 2026-01-01 (Thursday, +18 s): %d, %.1f", w, s);
        CHECK(std::fabs(gpsToUnix(w, s, 18) - 1767225600.0) < 1e-6, "week/tow round trip");
        CHECK(fixTrimbleWeek(2399) == 2399 && fixTrimbleWeek(375) == 2423 && fixTrimbleWeek(5000) == -1, "week roll-over repair: %d %d", fixTrimbleWeek(375), fixTrimbleWeek(2399));
    }
    if (fails()) { std::printf("%d failures\n", fails()); return 1; }
    std::printf("sonde bits KAT: ok\n");
    return 0;
}
