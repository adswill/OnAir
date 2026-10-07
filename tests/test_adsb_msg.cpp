// ADS-B message layer: known answers from published examples (CRC, identification, CPR, altitude codes, velocity, surface, Mode S replies, Comm-B).
// Examples: "The 1090 Megahertz Riddle" (Junzi Sun, https://mode-s.org/1090mhz, chapters ADS-B 1-8 and Mode S 2, 3, 6, 7) unless a comment says otherwise.
// The expected numbers below are the ones printed there; a hex string that does not give a zero CRC would show up as a failure here first.
#include "dect2/adsb_msg.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <vector>
using namespace dect2::adsb;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Hex {
    uint8_t b[14] = {};
    int bits = 0;
    explicit Hex(const char* s) {
        const int n = (int)strlen(s) / 2;
        for (int i = 0; i < n; i++) { unsigned v; sscanf(s + 2 * i, "%2x", &v); b[i] = (uint8_t)v; }
        bits = n * 8;
    }
};

static void testCrc() {
    // Riddle, ADS-B chapter 8 (error control): message A has remainder 0, message B has remainder 16
    Hex a("8D406B902015A678D4D220AA4BDA"), b("8D4CA251204994B1C36E60A5343D");
    CHECK(crc24(a.b, a.bits) == 0, "message A remainder %06X", crc24(a.b, a.bits));
    CHECK(crc24(b.b, b.bits) == 16, "message B remainder %u (the Riddle says 16)", crc24(b.b, b.bits));
    // the parity of message A is AA4BDA (the Riddle shows it as the result of the division of the data with 24 zero bits)
    CHECK(crcParity(a.b, a.bits) == 0xAA4BDA, "parity of message A %06X", crcParity(a.b, a.bits));
    const char* good[] = {
        "8D4840D6202CC371C32CE0576098",   // identification
        "8D40621D58C382D690C8AC2863A7", "8D40621D58C386435CC412692AD6",   // airborne position pair
        "8D485020994409940838175B284F", "8DA05F219B06B6AF189400CBC33F",   // velocity
        "8C4841753AAB238733C8CD4020B1", "8C4841753A8A35323FAEBDAC702D", "8C4841753A9A153237AEF0F275BE"};   // surface
    for (const char* s : good) { Hex h(s); CHECK(crc24(h.b, h.bits) == 0, "%s remainder %06X", s, crc24(h.b, h.bits)); }
    // the generator polynomial in the Riddle: 1111111111111010000001001 = 0x1FFF409
    CHECK(kCrcGenerator == 0xFFF409, "generator");
    // all-call reply, Riddle Mode S chapter 2: 5D484FDEA248F5, address 484FDE, remainder (interrogator code) 22
    Hex ac("5D484FDEA248F5");
    CHECK(crc24(ac.b, ac.bits) == 22, "DF11 remainder %u", crc24(ac.b, ac.bits));
    Msg m;
    CHECK(decodeMsg(ac.b, ac.bits, m) && m.df == 11 && m.ca == 5 && m.icao == 0x484FDE, "DF11 fields df %d ca %d icao %06X", m.df, m.ca, m.icao);
}

static void testSyndromes() {
    for (int n : {56, 112}) {
        std::set<uint32_t> seen;
        for (int p = 0; p < n; p++) {
            const uint32_t s = singleBitSyndrome(n, p);
            CHECK(s != 0 && seen.insert(s).second, "syndrome of bit %d of %d is not unique", p, n);
            CHECK(singleBitPosition(n, s) == p, "lookup of bit %d of %d gives %d", p, n, singleBitPosition(n, s));
        }
        CHECK(singleBitPosition(n, 0) == -1, "remainder 0 maps to a bit");
    }
    // a corrupted known message is repaired by the syndrome of the flipped bit
    Hex h("8D4840D6202CC371C32CE0576098");
    for (int p = 0; p < 112; p++) {
        Hex c = h;
        c.b[p >> 3] ^= (uint8_t)(0x80 >> (p & 7));
        CHECK(singleBitPosition(112, crc24(c.b, 112)) == p, "bit %d not located", p);
    }
}

static void testIdentification() {
    Hex h("8D4840D6202CC371C32CE0576098");   // Riddle ADS-B 1 and 2: ICAO 4840D6, callsign KLM1023, DF 17, TC 4, CA 0 (no category information)
    Msg m;
    CHECK(decodeMsg(h.b, h.bits, m), "decode");
    CHECK(m.df == 17 && m.icao == 0x4840D6 && m.tc == 4 && m.hasIdent, "df %d icao %06X tc %d", m.df, m.icao, m.tc);
    CHECK(m.callsign == "KLM1023", "callsign '%s'", m.callsign.c_str());
    CHECK(m.catSet == 'A' && m.catCode == 0, "category %c%d", m.catSet, m.catCode);
    // 6 bit character set: A - Z are 1 - 26, 0 - 9 are 48 - 57, space is 32 (Riddle ADS-B 2)
    CHECK(charFrom6bit(1) == 'A' && charFrom6bit(26) == 'Z' && charFrom6bit(48) == '0' && charFrom6bit(57) == '9' && charFrom6bit(32) == ' ' && charFrom6bit(0) == '#', "charset");
    for (char c = 'A'; c <= 'Z'; c++) CHECK(charFrom6bit((unsigned)charTo6bit(c)) == c, "round trip %c", c);
    for (char c = '0'; c <= '9'; c++) CHECK(charFrom6bit((unsigned)charTo6bit(c)) == c, "round trip %c", c);
    CHECK(categoryCode(4, 3) == "A3" && categoryCode(3, 1) == "B1" && categoryCode(2, 2) == "C2" && categoryCode(1, 5) == "D5", "category codes");
}

static void testAirbornePosition() {
    // Riddle ADS-B 3: even frame (F = 0) and odd frame (F = 1) of 40621D, 38000 ft
    Hex e("8D40621D58C382D690C8AC2863A7"), o("8D40621D58C386435CC412692AD6");
    Msg me, mo;
    CHECK(decodeMsg(e.b, e.bits, me) && decodeMsg(o.b, o.bits, mo), "decode");
    CHECK(me.icao == 0x40621D && me.tc == 11 && me.hasCpr && !me.cprOdd && mo.cprOdd, "frame types");
    CHECK(me.hasAlt && me.altFt == 38000 && mo.hasAlt && mo.altFt == 38000, "altitude %d / %d", me.altFt, mo.altFt);
    CHECK(me.cprLat == 93000 && me.cprLon == 51372, "even cpr %d %d", me.cprLat, me.cprLon);    // 0b10110101101001000, 0b01100100010101100
    CHECK(mo.cprLat == 74158 && mo.cprLon == 50194, "odd cpr %d %d", mo.cprLat, mo.cprLon);     // 0b10010000110101110, 0b01100010000010010
    double lat = 0, lon = 0;
    CHECK(cprGlobalAirborne(me.cprLat, me.cprLon, mo.cprLat, mo.cprLon, false, lat, lon), "global (even latest)");
    CHECK(std::fabs(lat - 52.25720214843750) < 1e-9 && std::fabs(lon - 3.91937255859375) < 1e-9, "even latest: %.11f %.11f", lat, lon);   // as printed in the Riddle
    CHECK(cprGlobalAirborne(me.cprLat, me.cprLon, mo.cprLat, mo.cprLon, true, lat, lon), "global (odd latest)");
    CHECK(std::fabs(lat - 52.26578017412606) < 1e-9, "odd latest lat %.11f (Riddle 52.26578017412606)", lat);
    // local decoding with a reference 100 NM away must give the same positions as the global decode
    double glat, glon;
    cprGlobalAirborne(me.cprLat, me.cprLon, mo.cprLat, mo.cprLon, true, glat, glon);
    CHECK(cprLocalAirborne(mo.cprLat, mo.cprLon, true, 51.0, 5.0, lat, lon) && std::fabs(lat - glat) < 1e-9 && std::fabs(lon - glon) < 1e-9, "local odd %.9f %.9f vs %.9f %.9f", lat, lon, glat, glon);
    cprGlobalAirborne(me.cprLat, me.cprLon, mo.cprLat, mo.cprLon, false, glat, glon);
    CHECK(cprLocalAirborne(me.cprLat, me.cprLon, false, 53.0, 3.0, lat, lon) && std::fabs(lat - glat) < 1e-9 && std::fabs(lon - glon) < 1e-9, "local even %.9f %.9f", lat, lon);
}

static void testNl() {
    // thresholds of the NL table as printed in readsb cpr.c (the DO-260B table): first six and last six
    const double thr[][2] = {{10.47047130, 59}, {14.82817437, 58}, {18.18626357, 57}, {21.02939493, 56}, {23.54504487, 55}, {25.82924707, 54},
                             {83.07199445, 7}, {83.99173563, 6}, {84.89166191, 5}, {85.75541621, 4}, {86.53536998, 3}, {87.00000000, 2}};
    for (auto& t : thr) {
        CHECK(cprNl(t[0] - 1e-6) == (int)t[1], "NL just below %.8f is %d, expected %d", t[0], cprNl(t[0] - 1e-6), (int)t[1]);
        CHECK(cprNl(t[0] + 1e-6) == (int)t[1] - 1, "NL just above %.8f is %d, expected %d", t[0], cprNl(t[0] + 1e-6), (int)t[1] - 1);
    }
    CHECK(cprNl(0) == 59 && cprNl(-0.0) == 59 && cprNl(52.2572) == 36 && cprNl(-52.2572) == 36 && cprNl(87.5) == 1 && cprNl(-90) == 1 && cprNl(86.9) == 2, "NL special values");
    int last = 60;
    for (double l = 0; l <= 90; l += 0.01) { const int n = cprNl(l); CHECK(n <= last, "NL grows at %.2f", l); last = n; }
}

static void testCprRoundTrip() {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> la(-85, 85), lo(-180, 180);
    int bad = 0, total = 0;
    for (int i = 0; i < 20000; i++) {
        const double lat = la(rng), lon = lo(rng);
        int le, xe, lo_, xo;
        cprEncodeAirborne(lat, lon, false, le, xe);
        cprEncodeAirborne(lat, lon, true, lo_, xo);
        double rl, rn;
        total++;
        if (!cprGlobalAirborne(le, xe, lo_, xo, i & 1, rl, rn)) { bad++; continue; }   // zone boundary: the standard says to wait for a new pair
        double dl = std::fabs(rl - lat), dn = std::fabs(std::remainder(rn - lon, 360.0)) * std::cos(lat * M_PI / 180);
        CHECK(dl < 6e-5 && dn < 1.2e-4, "airborne round trip %.5f %.5f -> %.5f %.5f", lat, lon, rl, rn);
        double ll, ln;
        CHECK(cprLocalAirborne((i & 1) ? lo_ : le, (i & 1) ? xo : xe, i & 1, lat + 1.5, lon - 2.0, ll, ln), "local");
        CHECK(std::fabs(ll - rl) < 1e-9 && std::fabs(std::remainder(ln - rn, 360.0)) < 1e-9, "local %.6f %.6f vs global %.6f %.6f", ll, ln, rl, rn);
    }
    CHECK(bad < total / 50, "%d of %d pairs rejected", bad, total);
    // surface: positions within the reference's 45 NM
    for (int i = 0; i < 5000; i++) {
        const double lat = la(rng), lon = lo(rng);
        int le, xe, lo_, xo;
        cprEncodeSurface(lat, lon, false, le, xe);
        cprEncodeSurface(lat, lon, true, lo_, xo);
        double rl, rn;
        if (!cprGlobalSurface(le, xe, lo_, xo, i & 1, lat + 0.3, lon - 0.4, rl, rn)) continue;
        CHECK(std::fabs(rl - lat) < 2e-5 && std::fabs(std::remainder(rn - lon, 360.0)) * std::cos(lat * M_PI / 180) < 4e-5, "surface %.5f %.5f -> %.5f %.5f", lat, lon, rl, rn);
        double ll, ln;
        CHECK(cprLocalSurface((i & 1) ? lo_ : le, (i & 1) ? xo : xe, i & 1, lat - 0.2, lon + 0.2, ll, ln) && std::fabs(ll - rl) < 1e-9 && std::fabs(std::remainder(ln - rn, 360.0)) < 1e-9, "surface local");
    }
}

static void testVelocity() {
    Msg m;
    {   // Riddle ADS-B 5, message A: subtype 1, ground speed 159 kt, track 182.88 degrees, -832 ft/min, GNSS 550 ft above baro
        Hex h("8D485020994409940838175B284F");
        CHECK(decodeMsg(h.b, h.bits, m) && m.tc == 19 && m.st == 1 && m.hasVel && m.speedKind == 0, "velocity A fields");
        CHECK(std::fabs(m.speedKt - 159.0) < 0.5, "speed %.2f", m.speedKt);
        CHECK(m.hasHeading && m.headingIsTrack && std::fabs(m.headingDeg - 182.88) < 0.01, "track %.3f", m.headingDeg);
        CHECK(m.hasVrate && m.vrateFpm == -832, "vrate %d", m.vrateFpm);
        CHECK(m.hasGnssDiff && m.gnssDiffFt == 550, "gnss diff %d", m.gnssDiffFt);
    }
    {   // message B: subtype 3, true airspeed 375 kt, heading 243.98, -2304 ft/min, no GNSS difference
        Hex h("8DA05F219B06B6AF189400CBC33F");
        CHECK(decodeMsg(h.b, h.bits, m) && m.tc == 19 && m.st == 3 && m.hasVel, "velocity B fields");
        CHECK(m.speedKind == 2 && std::fabs(m.speedKt - 375.0) < 0.5, "airspeed %.2f kind %d", m.speedKt, m.speedKind);
        CHECK(m.hasHeading && !m.headingIsTrack && std::fabs(m.headingDeg - 243.98) < 0.01, "heading %.3f", m.headingDeg);
        CHECK(m.hasVrate && m.vrateFpm == -2304, "vrate %d", m.vrateFpm);
        CHECK(!m.hasGnssDiff, "gnss diff reported");
    }
}

static void testSurface() {
    // Riddle ADS-B 4: pair (even first, odd latest) with the reference 51.990 / 4.375, then a local decode from the result
    Hex e("8C4841753AAB238733C8CD4020B1"), o("8C4841753A8A35323FAEBDAC702D"), l("8C4841753A9A153237AEF0F275BE");
    Msg me, mo, ml;
    CHECK(decodeMsg(e.b, e.bits, me) && decodeMsg(o.b, o.bits, mo) && decodeMsg(l.b, l.bits, ml), "decode");
    CHECK(me.surface && me.ground && me.cprLat == 115609 && me.cprLon == 116941 && !me.cprOdd, "even %d %d", me.cprLat, me.cprLon);
    CHECK(mo.surface && mo.cprLat == 39199 && mo.cprLon == 110269 && mo.cprOdd, "odd %d %d", mo.cprLat, mo.cprLon);
    double lat = 0, lon = 0;
    CHECK(cprGlobalSurface(me.cprLat, me.cprLon, mo.cprLat, mo.cprLon, true, 51.990, 4.375, lat, lon), "global surface");
    CHECK(std::fabs(lat - 52.320607) < 5e-7 && std::fabs(lon - 4.734735) < 5e-7, "global %.7f %.7f (Riddle 52.320607 4.734735)", lat, lon);
    double l2, n2;
    CHECK(cprLocalSurface(ml.cprLat, ml.cprLon, ml.cprOdd, lat, lon, l2, n2), "local surface");
    CHECK(std::fabs(l2 - 52.320561) < 5e-7 && std::fabs(n2 - 4.735735) < 5e-7, "local %.7f %.7f (Riddle 52.320561 4.735735)", l2, n2);
    // movement 17 kt (code 41) and ground track 92.8 degrees (code 33), also from the Riddle
    CHECK(ml.hasMove && std::fabs(ml.moveKt - 17.0) < 1e-9, "movement %.3f", ml.moveKt);
    CHECK(ml.hasTrack && std::fabs(ml.trackDeg - 92.8125) < 1e-9, "track %.4f", ml.trackDeg);
}

static void testSurveillanceReplies() {
    Msg m;
    {   // Riddle Mode S 3: altitude reply 36000 ft, identity reply squawk 0356
        Hex h("2000171806A983");
        CHECK(decodeMsg(h.b, h.bits, m) && m.df == 4 && m.hasAlt && m.altFt == 36000 && !m.ground && !m.alert, "DF4 alt %d", m.altFt);
    }
    {
        Hex h("2A00516D492B80");
        CHECK(decodeMsg(h.b, h.bits, m) && m.df == 5 && m.hasSquawk && m.squawk == 356, "DF5 squawk %d", m.squawk);
    }
    // identity code layout round trip over all 4096 codes
    for (int a = 0; a < 8; a++) for (int b = 0; b < 8; b++) for (int c = 0; c < 8; c++) for (int d = 0; d < 8; d++) {
        const int sq = a * 1000 + b * 100 + c * 10 + d;
        CHECK(decodeId13(encodeId13(sq)) == sq, "squawk %04d", sq);
    }
    CHECK(decodeId13(encodeId13(7700)) == 7700 && decodeId13(encodeId13(1200)) == 1200, "emergency squawk");
}

static void testGillham() {
    // en.wikipedia.org/wiki/Gillham_code: D1 D2 D4 A1 A2 A4 B1 B2 B4 C1 C2 C4 = 000 000 011 010 -> 0 ft, ...011 110 -> 100 ft, 000 000 010 001 -> 700 ft
    auto code = [](int d2, int d4, int a1, int a2, int a4, int b1, int b2, int b4, int c1, int c2, int c4) {
        return (unsigned)((c1 << 10) | (a1 << 9) | (c2 << 8) | (a2 << 7) | (c4 << 6) | (a4 << 5) | (b1 << 4) | (b2 << 3) | (d2 << 2) | (b4 << 1) | d4);
    };
    int alt = 0;
    CHECK(gillhamToAltitude(code(0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 0), alt) && alt == 0, "0 ft gives %d", alt);
    CHECK(gillhamToAltitude(code(0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0), alt) && alt == 100, "100 ft gives %d", alt);
    CHECK(gillhamToAltitude(code(0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1), alt) && alt == 700, "700 ft gives %d", alt);
    // every code that is defined gives one altitude, and the 1280 codes cover -1200 to 126700 ft in 100 ft steps (the range of the code)
    std::set<int> seen;
    int valid = 0;
    for (unsigned c = 0; c < 2048; c++) {
        if (gillhamToAltitude(c, alt)) { valid++; seen.insert(alt); CHECK(alt % 100 == 0, "altitude %d is not a multiple of 100", alt); }
    }
    CHECK(valid == 1280 && seen.size() == 1280 && *seen.begin() == -1200 && *seen.rbegin() == 126700, "valid %d distinct %zu range %d..%d", valid, seen.size(), *seen.begin(), *seen.rbegin());
    for (int a = -1200; a <= 126700; a += 100) {
        unsigned c;
        CHECK(altitudeToGillham(a, c) && gillhamToAltitude(c, alt) && alt == a, "Gillham round trip %d -> %d", a, alt);
    }
    // neighbouring altitudes differ in exactly one bit (it is a Gray-like code)
    for (int a = -1200; a < 126700; a += 100) {
        unsigned c1, c2;
        altitudeToGillham(a, c1); altitudeToGillham(a + 100, c2);
        CHECK(__builtin_popcount(c1 ^ c2) == 1, "codes of %d and %d differ in %d bits", a, a + 100, __builtin_popcount(c1 ^ c2));
    }
    // AC13 with Q = 0 carries the Gillham code, with Q = 1 a 25 ft count
    auto ac13 = [](unsigned c11) { return ((c11 & 0x7E0) << 2) | ((c11 & 0x10) << 1) | (c11 & 0xF); };   // insert M (bit 6) and Q (bit 4) as zeros
    unsigned c;
    altitudeToGillham(35000, c);
    CHECK(decodeAc13(ac13(c), alt) && alt == 35000, "AC13 Gillham 35000 gives %d", alt);
    CHECK(decodeAc13(0, alt) == false, "AC13 0 is no altitude");
    // Q = 1: 25 ft steps, N = 1560 -> 38000 ft (Riddle ADS-B 3)
    CHECK(decodeAlt12(0xC38, alt) && alt == 38000, "12 bit 0xC38 gives %d", alt);
    for (unsigned n = 0; n < 2048; n++) {   // every 11 bit count N put into the field by hand: altitude 25 N - 1000
        const unsigned f = ((n >> 5) << 7) | (((n >> 4) & 1) << 5) | 0x10 | (n & 0xF);
        CHECK(decodeAc13(f, alt) == (f != 0) && (f == 0 || alt == (int)n * 25 - 1000), "Q = 1 count %u gives %d", n, alt);
    }
    CHECK(decodeAc13(0x0040 | 0x0010 | 0x0100, alt) == false, "metric altitude is refused");
}

static void testComplexMessages() {
    Msg m;
    {   // Riddle Mode S 6 (ELS): BDS 2,0 in a DF20, callsign KLM1017
        Hex h("A000083E202CC371C31DE0AA1CCF");
        CHECK(decodeMsg(h.b, h.bits, m) && m.df == 20 && m.commb.bds == 0x20 && m.commb.callsign == "KLM1017", "BDS 2,0: bds %02X '%s'", m.commb.bds, m.commb.callsign.c_str());
    }
    {   // Riddle Mode S 7 (EHS) BDS 4,0: MCP/FCU 24000 ft, FMS 24000 ft, 1013.2 mb
        Hex h("A8001EBCAEE57730A80106DE1344");
        CHECK(decodeMsg(h.b, h.bits, m) && m.commb.bds == 0x40, "BDS 4,0 inferred %02X", m.commb.bds);
        CHECK(m.commb.hasSelAlt && m.commb.selAltMcpFt == 24000 && m.commb.hasSelAltFms && m.commb.selAltFmsFt == 24000, "BDS 4,0 altitudes %d %d", m.commb.selAltMcpFt, m.commb.selAltFmsFt);
        CHECK(m.commb.hasBaro && std::fabs(m.commb.baroMb - 1013.2) < 0.05, "BDS 4,0 baro %.2f", m.commb.baroMb);
    }
    {   // BDS 5,0: roll -9.7, track 140.273, ground speed 476 kt, track rate -0.406 deg/s, TAS 466 kt
        Hex h("A80006ACF9363D3BBF9CE98F1E1D");
        CHECK(decodeMsg(h.b, h.bits, m) && m.commb.bds == 0x50, "BDS 5,0 inferred %02X", m.commb.bds);
        CHECK(std::fabs(m.commb.rollDeg - (-9.668)) < 0.01 && std::fabs(m.commb.trackDeg - 140.273) < 0.001 && m.commb.gsKt == 476 && std::fabs(m.commb.trackRateDps - (-0.40625)) < 1e-4 && m.commb.tasKt == 466,
              "BDS 5,0 roll %.3f track %.3f gs %d rate %.4f tas %d", m.commb.rollDeg, m.commb.trackDeg, m.commb.gsKt, m.commb.trackRateDps, m.commb.tasKt);
    }
    {   // BDS 6,0: heading 110.391, IAS 259 kt, Mach 0.7, baro rate -2144, inertial rate -2016 ft/min
        Hex h("A80004AAA74A072BFDEFC1D5CB4F");
        CHECK(decodeMsg(h.b, h.bits, m) && m.commb.bds == 0x60, "BDS 6,0 inferred %02X", m.commb.bds);
        CHECK(std::fabs(m.commb.headingDeg - 110.391) < 0.001 && m.commb.iasKt == 259 && std::fabs(m.commb.mach - 0.7) < 1e-9 && m.commb.baroRateFpm == -2144 && m.commb.inertialRateFpm == -2016,
              "BDS 6,0 heading %.3f ias %d mach %.3f baro %d inertial %d", m.commb.headingDeg, m.commb.iasKt, m.commb.mach, m.commb.baroRateFpm, m.commb.inertialRateFpm);
    }
    {   // random data is not mistaken for a Comm-B format (only a few random fields pass the status and reserved bit checks)
        std::mt19937 rng(3);
        int hits = 0;
        for (int i = 0; i < 1000000; i++) {
            uint8_t mb[7];
            for (auto& x : mb) x = (uint8_t)rng();
            if (inferCommB(mb).bds) hits++;
        }
        CHECK(hits < 600, "%d of 1000000 random fields were taken for a Comm-B format", hits);   // measured 0.05 %; it was 0.17 % before the checks were tightened
    }
}

static void testExtendedStatus() {
    // Fields that are built here, not copied from a document: the positions were checked against readsb (target state) and the Riddle (operational status)
    Msg m;
    uint8_t b[14] = {};
    setBits(b, 1, 5, 17); setBits(b, 9, 24, 0xABCDEF);
    setBits(b, 33, 5, 28); setBits(b, 38, 3, 1); setBits(b, 41, 3, 4); setBits(b, 44, 13, encodeId13(7600));
    uint32_t p = crcParity(b, 112); setBits(b, 89, 24, p);
    CHECK(crc24(b, 112) == 0 && decodeMsg(b, 112, m) && m.tc == 28 && m.emergency == 4 && m.hasSquawk && m.squawk == 7600, "TC 28: emergency %d squawk %d", m.emergency, m.squawk);
    CHECK(std::string(emergencyName(4)) == "No communications" && std::string(emergencyName(0)) == "None", "emergency names");
}

static void testNoPosition() {
    // TC 0: altitude only (DO-260B: "no position information"); built here, the layout is that of the airborne position message
    Msg m;
    uint8_t b[14] = {};
    setBits(b, 1, 5, 17); setBits(b, 6, 3, 5); setBits(b, 9, 24, 0x123456);
    setBits(b, 33, 5, 0); setBits(b, 41, 12, 0xC38);      // 38000 ft with the Q bit, as in the Riddle's position example
    setBits(b, 89, 24, crcParity(b, 112));
    CHECK(crc24(b, 112) == 0 && decodeMsg(b, 112, m) && m.tc == 0 && m.hasAlt && m.altFt == 38000 && !m.hasCpr, "TC 0: alt %d", m.altFt);
    setBits(b, 41, 12, 0);                                  // no altitude either
    setBits(b, 89, 24, crcParity(b, 112));
    CHECK(decodeMsg(b, 112, m) && m.tc == 0 && !m.hasAlt, "TC 0 without altitude");
}

static void testGeo() {
    CHECK(std::fabs(distanceNm(0, 0, 0, 1) - 60.04) < 0.05, "one degree of longitude at the equator is %.3f NM", distanceNm(0, 0, 0, 1));
    CHECK(std::fabs(distanceNm(0, 0, 1, 0) - 60.04) < 0.05, "one degree of latitude");
    CHECK(std::fabs(bearingDeg(0, 0, 0, 1) - 90.0) < 1e-9 && std::fabs(bearingDeg(0, 0, 1, 0)) < 1e-9 && std::fabs(bearingDeg(1, 0, 0, 0) - 180.0) < 1e-9 && std::fabs(bearingDeg(0, 1, 0, 0) - 270.0) < 1e-9, "bearings");
}

int main() {
    testCrc();
    testSyndromes();
    testIdentification();
    testAirbornePosition();
    testNl();
    testCprRoundTrip();
    testVelocity();
    testSurface();
    testSurveillanceReplies();
    testGillham();
    testComplexMessages();
    testExtendedStatus();
    testNoPosition();
    testGeo();
    printf(fails ? "adsb_msg: %d FAILED\n" : "adsb_msg: all passed\n", fails);
    return fails ? 1 : 0;
}
