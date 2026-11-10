// Golden-vector test of the Rust ADS-B / Mode S message layer (rust/onair_adsb, dect2::adsb): every code of the altitude / identity / character tables, CRC and
// single-bit correction on frames and damaged frames, CPR with real and absurd values (NaN references too), geometry, Comm-B registers built to pass each BDS
// inference plus damaged ones, and decoded messages of every DF and type code. ONAIR_DIFF_ITERS (default 200000) seeded random inputs per family. Every result
// goes into a digest (tests/common/golden.h; doubles rounded to 1e-6) compared with the one recorded from the C++ message layer the Rust code replaced.
#include "dect2/adsb/adsb_msg.h"
#include "../common/golden.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <random>
namespace A = dect2::adsb;

static golden::Dual dig;

static std::mt19937_64 g(20261016);

static void addCommb(const A::CommB& a) {
    dig.add(a.bds, a.callsign, a.hasSelAlt, a.selAltMcpFt, a.hasSelAltFms, a.selAltFmsFt);
    dig.add(a.hasBaro, a.baroMb, a.hasRoll, a.rollDeg, a.hasTrack, a.trackDeg);
    dig.add(a.hasGs, a.gsKt, a.hasTrackRate, a.trackRateDps, a.hasTas, a.tasKt);
    dig.add(a.hasHeading, a.headingDeg, a.hasIas, a.iasKt, a.hasMach, a.mach);
    dig.add(a.hasBaroRate, a.baroRateFpm, a.hasInertialRate, a.inertialRateFpm, a.subnetVersion, a.acasOperating);
    dig.add(a.continuation, a.gicbMask, a.ara, a.rac, a.raTerminated, a.multipleThreats);
    dig.add(a.tti, a.threatIcao, a.fom, a.hasWind, a.windKt, a.windDirDeg);
    dig.add(a.hasSat, a.satC, a.hasPressure, a.pressureHpa, a.hasHumidity, a.humidityPct);
    dig.add(a.turbulence, a.windShear, a.microburst, a.icing, a.wakeVortex, a.hasRadioHeight);
    dig.add(a.radioHeightFt);
}
static void addMsg(const A::Msg& a) {
    addCommb(a.commb);
    dig.add(a.df, a.nbits, a.icao, a.ca, a.ground, a.alert);
    dig.add(a.spi, a.fs, a.vs, a.hasAlt, a.altFt, a.altGnss);
    dig.add(a.hasSquawk, a.squawk, a.tc, a.st, a.hasIdent, a.callsign);
    dig.add(a.catSet, a.catCode, a.hasCpr, a.cprOdd, a.surface, a.cprLat);
    dig.add(a.cprLon, a.hasMove, a.moveKt, a.hasTrack, a.trackDeg, a.hasVel);
    dig.add(a.speedKt, a.speedKind, a.hasHeading, a.headingDeg, a.headingIsTrack, a.hasVrate);
    dig.add(a.vrateFpm, a.vrateBaro, a.hasGnssDiff, a.gnssDiffFt, a.emergency, a.hasSelAlt);
    dig.add(a.selAltFt, a.selAltFms, a.hasBaro, a.baroMb, a.hasSelHdg, a.selHdgDeg);
    dig.add(a.tsNacp, a.autopilot, a.vnav, a.altHold, a.approach, a.lnav);
    dig.add(a.tsModeValid, a.adsbVersion, a.nacp, a.nicSuppA, a.sil, a.gva);
    dig.add(a.nicBaro, a.callsign, a.commb.bds);
}

using Bytes = std::vector<uint8_t>;
static Bytes randBytes(size_t n) { Bytes v(n); for (auto& c : v) c = (uint8_t)g(); return v; }
static double rndLat() { return (double)(int64_t)(g() % 18000001) / 100000.0 - 90.0; }
static double rndLon() { return (double)(int64_t)(g() % 36000001) / 100000.0 - 180.0; }
static double odd() {   // absurd doubles now and then
    static const double v[] = {std::numeric_limits<double>::quiet_NaN(), INFINITY, -INFINITY, 0.0, -0.0, 90.0, -90.0, 180.0, -180.0, 360.0, 1e300, -1e300, 1e-300, 89.999999, 87.0, -87.0};
    return v[g() % (sizeof v / sizeof v[0])];
}

// ---- Comm-B registers built to pass the checks of one BDS code (then maybe damaged)
static void setb(Bytes& mb, int first, int count, uint32_t v) { A::setBits(mb.data(), first, count, v); }
static uint32_t rb(int bits) { return (uint32_t)(g() & ((1ull << bits) - 1)); }
static Bytes makeCommB(int kind) {
    Bytes mb(7, 0);
    switch (kind) {
    case 0: mb[0] = 0x20; { std::string s = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 "; for (int i = 0; i < 8; i++) setb(mb, 9 + 6 * i, 6, (uint32_t)A::charTo6bit(s[g() % s.size()])); } break;
    case 1: // 4,0
        setb(mb, 1, 1, 1); setb(mb, 2, 12, rb(12)); setb(mb, 14, 1, g() & 1); setb(mb, 15, 12, rb(12)); setb(mb, 27, 1, 1); setb(mb, 28, 12, 1000 + g() % 1000);
        if (!(mb[1] | mb[2])) setb(mb, 2, 12, 5);
        { if (g() % 3 == 0) setb(mb, 1, 1, 0), setb(mb, 2, 12, 0); } break;
    case 2: // 5,0
        for (int st : {1, 12, 24, 35, 46}) if (g() % 6) setb(mb, st, 1, 1);
        if (mb[0] & 0x80) setb(mb, 2, 10, rb(10) % 200);
        setb(mb, 13, 11, rb(11)); setb(mb, 25, 10, rb(10) % 300); setb(mb, 36, 10, rb(10)); setb(mb, 47, 10, rb(10) % 250);
        // zero the values whose status is 0
        if (!A::getBits(mb.data(), 1, 1)) setb(mb, 2, 10, 0);
        if (!A::getBits(mb.data(), 12, 1)) setb(mb, 13, 11, 0);
        if (!A::getBits(mb.data(), 24, 1)) setb(mb, 25, 10, 0);
        if (!A::getBits(mb.data(), 35, 1)) setb(mb, 36, 10, 0);
        if (!A::getBits(mb.data(), 46, 1)) setb(mb, 47, 10, 0);
        break;
    case 3: // 6,0
        for (int st : {1, 13, 24, 35, 46}) if (g() % 6) setb(mb, st, 1, 1);
        setb(mb, 2, 11, rb(11)); setb(mb, 14, 10, rb(10) % 400); setb(mb, 25, 10, rb(10) % 250); setb(mb, 36, 10, rb(10)); setb(mb, 47, 10, rb(10));
        if (!A::getBits(mb.data(), 1, 1)) setb(mb, 2, 11, 0);
        if (!A::getBits(mb.data(), 13, 1)) setb(mb, 14, 10, 0);
        if (!A::getBits(mb.data(), 24, 1)) setb(mb, 25, 10, 0);
        if (!A::getBits(mb.data(), 35, 1)) setb(mb, 36, 10, 0);
        if (!A::getBits(mb.data(), 46, 1)) setb(mb, 47, 10, 0);
        break;
    case 4: mb[0] = 0x10; setb(mb, 9, 1, g() & 1); setb(mb, 17, 7, g() % 7); setb(mb, 38, 1, g() & 1); setb(mb, 25, 13, rb(13) & 0x1fff); break;
    case 5: setb(mb, 1, 24, rb(24) | 0x200000); setb(mb, 7, 1, 1); if (g() % 4 == 0) setb(mb, 30, 3, rb(3)); break;
    case 6: // 3,0
        mb[0] = 0x30; setb(mb, 9, 14, rb(14)); setb(mb, 23, 4, rb(4) & 3); setb(mb, 27, 1, g() & 1); setb(mb, 28, 1, g() & 1); setb(mb, 29, 2, g() % 3);
        { int tti = (int)A::getBits(mb.data(), 29, 2); if (tti == 1) setb(mb, 31, 24, rb(24) | 1); else if (tti == 2) { setb(mb, 31, 13, rb(13)); setb(mb, 44, 7, 1 + g() % 100); setb(mb, 51, 6, 1 + g() % 60); } } break;
    case 7: // 4,4
        setb(mb, 1, 4, 1 + g() % 4); setb(mb, 5, 1, 1); setb(mb, 6, 9, rb(9) % 250); setb(mb, 15, 9, rb(9));
        { int sat = (int)(g() % 400) - 200; setb(mb, 24, 1, sat < 0); setb(mb, 25, 10, (uint32_t)sat & 0x3ff); }
        if (g() & 1) { setb(mb, 35, 1, 1); setb(mb, 36, 11, 100 + g() % 1000); }
        if (g() & 1) { setb(mb, 47, 1, 1); setb(mb, 48, 2, rb(2)); }
        if (g() & 1) { setb(mb, 50, 1, 1); setb(mb, 51, 6, rb(6)); }
        break;
    case 8: // 4,5
        for (int i = 0; i < 5; i++) if (g() & 1) { setb(mb, 1 + 3 * i, 1, 1); setb(mb, 2 + 3 * i, 2, rb(2)); }
        setb(mb, 16, 1, 1); { int sat = (int)(g() % 400) - 200; setb(mb, 17, 1, sat < 0); setb(mb, 18, 9, (uint32_t)sat & 0x1ff); }
        if (g() & 1) { setb(mb, 27, 1, 1); setb(mb, 28, 11, 100 + g() % 1000); }
        if (g() & 1) { setb(mb, 39, 1, 1); setb(mb, 40, 12, g() % 320); }
        if (!A::getBits(mb.data(), 1, 12)) setb(mb, 1, 1, 1), setb(mb, 2, 2, 1);
        break;
    default: mb = randBytes(7); break;
    }
    if (g() % 3 == 0) { const int pos = 1 + (int)(g() % 56); const uint32_t bit = g() & 1; setb(mb, pos, 1, bit); }   // draws in a fixed order (call arguments are not)
    return mb;
}

// ---- messages of every format
static Bytes makeMessage(int& nbits) {
    static const int dfs[] = {0, 4, 5, 11, 16, 17, 18, 19, 20, 21, 24, 25, 26, 27, 1, 3, 12, 22, 28, 31};
    const int df = dfs[g() % 20];
    nbits = A::dfLength(df) ? A::dfLength(df) : (g() & 1 ? 56 : 112);
    if (g() % 40 == 0) nbits = (g() & 1) ? 112 : 56;   // length that does not fit the DF
    Bytes m = randBytes((size_t)nbits / 8);
    A::setBits(m.data(), 1, 5, (uint32_t)df);
    if (df == 17 || df == 18 || df == 19) {
        static const int tcs[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 18, 19, 19, 19, 20, 22, 28, 29, 29, 31, 31, 0, 23, 24, 25, 26, 27, 30};
        const int tc = g() % 4 ? tcs[g() % (sizeof tcs / sizeof tcs[0])] : (int)(g() % 33);
        A::setBits(m.data(), 33, 5, (uint32_t)(tc & 31));
        if (g() % 3 == 0) A::setBits(m.data(), 6, 3, g() % 8 < 5 ? 0 : (uint32_t)(4 + g() % 4));
        if (tc == 19) A::setBits(m.data(), 38, 3, g() % 7);
        if (tc == 29) A::setBits(m.data(), 38, 2, g() % 4);
        if (tc == 31 || tc == 28) A::setBits(m.data(), 38, 3, g() % 4);
        if (tc >= 9 && tc <= 22 && g() % 2) A::setBits(m.data(), 48, 1, 1);   // Q bit of the altitude
    }
    if (df == 4 || df == 20) { if (g() % 2) A::setBits(m.data(), 20, 13, (A::getBits(m.data(), 20, 13) | 0x10) & ~0x40u); }
    if (df == 20 || df == 21) {
        const Bytes mb = makeCommB((int)(g() % 10));
        for (int i = 0; i < 7 && 4 + (size_t)i < m.size(); i++) m[4 + (size_t)i] = mb[(size_t)i];   // m is 56 bits long when the length was made not to fit the DF
    }
    return m;
}

int main() {
    const char* it = getenv("ONAIR_DIFF_ITERS");
    const long iters = it ? atol(it) : 200000;

    // CRC and single-bit correction
    for (int df = -5; df < 40; df++) dig.add(A::dfLength(df));
    for (int n : {56, 112}) for (int p = 0; p < n; p++) {
        const uint32_t s = A::singleBitSyndrome(n, p);
        dig.add(s, A::singleBitPosition(n, s));
        if (A::singleBitPosition(n, s) != p) { printf("singleBitPosition %d %d is not the inverse\n", n, p); return 1; }
    }
    for (long i = 0; i < iters; i++) {
        const int n = g() & 1 ? 112 : 56;
        const uint32_t rbits = (uint32_t)g(); const uint32_t r = rbits & (g() & 1 ? 0xFFFFFFu : 0xFFFFFFFFu);
        dig.add(A::singleBitPosition(n, r));
        Bytes m = randBytes((size_t)n / 8);
        dig.add(A::crc24(m.data(), n));
        dig.add(A::crcParity(m.data(), n));
        // a valid frame, then one or two flipped bits: the remainder, and the position it points at
        const uint32_t par = A::crcParity(m.data(), n);
        m[(size_t)n / 8 - 3] = (uint8_t)(par >> 16); m[(size_t)n / 8 - 2] = (uint8_t)(par >> 8); m[(size_t)n / 8 - 1] = (uint8_t)par;
        dig.add(A::crc24(m.data(), n));
        const int p = (int)(g() % (uint64_t)n);
        m[(size_t)p / 8] ^= (uint8_t)(0x80 >> (p & 7));
        const uint32_t rem = A::crc24(m.data(), n);
        dig.add(rem, A::singleBitPosition(n, rem));
        const int q = (int)(g() % (uint64_t)n);
        m[(size_t)q / 8] ^= (uint8_t)(0x80 >> (q & 7));
        dig.add(A::crc24(m.data(), n));
        const uint32_t r2 = A::crc24(m.data(), n);
        dig.add(A::singleBitPosition(n, r2));
    }

    // codes: every value of the small tables
    for (unsigned v = 0; v < 0x4000; v++) {
        int a = 12345;
        const bool r1 = A::decodeAc13(v, a);
        dig.add(r1); if (r1) dig.add(a);
        a = 12345;
        const bool s1 = A::decodeAlt12(v, a);
        dig.add(s1); if (s1) dig.add(a);
        dig.add(A::decodeId13(v));
        const bool t1 = A::gillhamToAltitude(v, a = 12345);
        dig.add(t1); if (t1) dig.add(a);
    }
    for (unsigned v : {0x7FFFu, 0xFFFFu, 0x10000u, 0xFFFFFFFFu, 0x80000000u, 0x12345678u}) {
        int a = 0;
        dig.add(A::decodeAc13(v, a), a);
        dig.add(A::decodeAlt12(v, a), a);
        dig.add(A::decodeId13(v));
    }
    for (int alt = -3000; alt <= 130000; alt++) {
        unsigned a = 7;
        const bool r1 = A::altitudeToGillham(alt, a);
        dig.add(r1); if (r1) dig.add(a);
    }
    for (int sq = -2000; sq <= 12000; sq++) dig.add(A::encodeId13(sq));
    for (int v : {-2147483647 - 1, 2147483647, 99999, 100000, 7777777}) dig.add(A::encodeId13(v));
    for (unsigned c = 0; c < 400; c++) dig.add(A::charFrom6bit(c));
    for (int c = 0; c < 256; c++) dig.add(A::charTo6bit((char)c));
    for (int tc = -2; tc < 9; tc++) for (int ca = -2; ca < 10; ca++) {
        dig.add(A::categoryCode(tc, ca));
        if (ca >= 0 && ca < 8) dig.add(A::categoryName(tc, ca));
    }
    for (int s = -3; s < 11; s++) dig.add(A::emergencyName(s));

    // CPR: zone counts over a latitude sweep, real positions, random 17 bit values, absurd references
    for (int i = -100000; i <= 100000; i++) { const double lat = i * 0.0009; dig.add(A::cprNl(lat)); }
    for (int i = 0; i < 100; i++) { const double v = odd(); dig.add(A::cprNl(v)); }
    for (double lat : {(double)NAN, (double)INFINITY, -(double)INFINITY, 87.0, 86.9999999, 10.47047130, 14.82817437, 87.00000001}) dig.add(A::cprNl(lat));
    long cprGlobal = 0, cprLocal = 0, cprSurf = 0;
    for (long i = 0; i < iters; i++) {
        const bool useReal = g() % 2;
        const double lat = rndLat(), lon = rndLon();
        int le, xe, lo, xo;
        if (useReal) { A::cprEncodeAirborne(lat, lon, false, le, xe); A::cprEncodeAirborne(lat, lon, true, lo, xo); dig.add(le, xe, lo, xo); }
        else { le = (int)(g() % 131072); xe = (int)(g() % 131072); lo = (int)(g() % 131072); xo = (int)(g() % 131072); }
        const bool oddLatest = g() & 1;
        double rlat = g() % 8 == 0 ? odd() : lat + ((double)(g() % 2000) / 1000.0 - 1), rlon = g() % 8 == 0 ? odd() : lon + ((double)(g() % 2000) / 1000.0 - 1);
        double a1 = 11, a2 = 12;
        bool r1 = A::cprGlobalAirborne(le, xe, lo, xo, oddLatest, a1, a2);
        cprGlobal += r1;
        dig.add(r1); if (r1) dig.add(a1, a2);
        const bool o = g() & 1;
        a1 = 11; a2 = 12;
        r1 = A::cprLocalAirborne(le, xe, o, rlat, rlon, a1, a2);
        cprLocal += r1;
        dig.add(r1); if (r1) dig.add(a1, a2);
        a1 = 11; a2 = 12;
        r1 = A::cprGlobalSurface(le, xe, lo, xo, oddLatest, rlat, rlon, a1, a2);;
        cprSurf += r1;
        dig.add(r1); if (r1) dig.add(a1, a2);
        a1 = 11; a2 = 12;
        r1 = A::cprLocalSurface(lo, xo, o, rlat, rlon, a1, a2);;
        dig.add(r1); if (r1) dig.add(a1, a2);
        int e1, e2;
        A::cprEncodeSurface(lat, lon, o, e1, e2);
        dig.add(e1, e2);
    }

    // geometry and models
    for (long i = 0; i < iters; i++) {
        const double la1 = g() % 20 == 0 ? odd() : rndLat(), lo1 = g() % 20 == 0 ? odd() : rndLon(), la2 = g() % 20 == 0 ? odd() : rndLat(), lo2 = g() % 20 == 0 ? odd() : rndLon();
        dig.add(A::distanceNm(la1, lo1, la2, lo2));
        dig.add(A::bearingDeg(la1, lo1, la2, lo2));
        dig.add(A::magDeclinationDeg(la1, lo1));
        const double trk = (double)(g() % 3600) / 10, gs = (double)(g() % 6000) / 10, hdg = (double)(g() % 3600) / 10, tas = (double)(g() % 6000) / 10, mach = (double)(g() % 1000) / 1000 - 0.1;
        double f1 = 1, k1 = 2;
        A::windTriangle(trk, gs, hdg, tas, f1, k1);
        dig.add(f1, k1);
        dig.add(A::satFromTasMach(tas, mach));
        const double alt = (double)((int64_t)(g() % 600000) - 100000) / 10.0;
        dig.add(A::isaPressureHpa(alt), A::isaTempC(alt));
    }
    for (unsigned ara = 0; ara < 16384; ara++) for (unsigned rac = 0; rac < 16; rac++) for (int t = 0; t < 2; t++)
        if (t == 0 || (ara & 0xF) == 0) dig.add(A::raText(ara, rac, t));

    // Comm-B registers
    std::map<int, long> found;
    for (long i = 0; i < iters * 2; i++) {
        const Bytes mb = makeCommB((int)(g() % 10));
        const int alt = g() % 3 == 0 ? A::kNoAltitude : (int)(g() % 50000) - 1000;
        const A::CommB a = A::inferCommB(mb.data(), alt);
        found[a.bds]++;
        addCommb(a);
    }
    for (long i = 0; i < iters; i++) {   // weather registers need a matching altitude: pick the altitude from the temperature that was built
        Bytes mb = makeCommB(7 + (int)(g() % 2));
        const int alt = (int)(g() % 40000);
        const A::CommB a = A::inferCommB(mb.data(), alt);
        found[a.bds]++;
        addCommb(a);
    }
    for (long i = 0; i < iters / 4; i++) {   // fully random fields and the all-zero field
        const Bytes mb = i % 10 ? randBytes(7) : Bytes(7, 0);
        addCommb(A::inferCommB(mb.data(), A::kNoAltitude));
    }

    // decoded messages
    long decodedOk = 0, withPos = 0, withVel = 0, withCommB = 0;
    for (long i = 0; i < iters * 3; i++) {
        int nbits;
        Bytes m = makeMessage(nbits);
        if (g() % 5 == 0) { const int p = (int)(g() % (uint64_t)nbits); m[(size_t)p / 8] ^= (uint8_t)(0x80 >> (p & 7)); }
        A::Msg a;
        const bool r1 = A::decodeMsg(m.data(), nbits, a);
        decodedOk += r1; withPos += a.hasCpr; withVel += a.hasVel; withCommB += a.commb.bds != 0;
        dig.add(r1); addMsg(a);
    }
    // a few real frames of the Riddle
    for (const char* hx : {"8D4840D6202CC371C32CE0576098", "8D40621D58C382D690C8AC2863A7", "8D40621D58C386435CC412692AD6", "8D485020994409940838175B284F", "8D4840D6202CC371C32CE0576098"}) {
        Bytes m(14);
        for (int i = 0; i < 14; i++) m[(size_t)i] = (uint8_t)strtol(std::string(hx + 2 * i, 2).c_str(), nullptr, 16);
        A::Msg a;
        dig.add(A::decodeMsg(m.data(), 112, a)); addMsg(a);
        dig.add(A::toHex(m.data(), 112));
    }

    printf("  coverage: %ld global CPR fixes, %ld local, %ld surface; %ld messages decoded (%ld with a CPR frame, %ld with a velocity, %ld with a Comm-B register)\n", cprGlobal, cprLocal, cprSurf, decodedOk, withPos, withVel, withCommB);
    printf("  Comm-B inference results:");
    for (auto& kv : found) printf(" %02x:%ld", kv.first, kv.second);
    printf("\n");
    // re-recorded 2026-11-10: DF18 control field 3 (coarse TIS-B) is no longer read as an ADS-B ME, the operational status capability and NACv, and the RA broadcast (TC 28 subtype 2) fill fields that were empty (was 0xccbaa93e37f1e80d, 0x486a022706e9ea78)
    return golden::checkDual("adsb_proto_rs", dig, 0x8b2ea87a44285088ull, 0x8c063a53f2b340dcull, !it);
}
