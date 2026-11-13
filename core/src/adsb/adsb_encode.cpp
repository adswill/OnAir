// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ADS-B / Mode S message encoders (see adsb_encode.h).
#include "dect2/adsb/adsb_encode.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2::adsb {

static void finishPi(Frame& f) {   // pure parity: DF11 (interrogator code 0), DF17, DF18
    setBits(f.b, f.bits - 23, 24, crcParity(f.b, f.bits));
}

static void finishAp(Frame& f, uint32_t icao) {   // address / parity: the address is XORed into the parity
    setBits(f.b, f.bits - 23, 24, crcParity(f.b, f.bits) ^ (icao & 0xFFFFFF));
}

unsigned encodeAc13(int altFt, bool gillham) {
    if (gillham) {
        unsigned c;
        if (!altitudeToGillham(altFt, c)) return 0;
        return ((c & 0x7E0) << 2) | ((c & 0x10) << 1) | (c & 0xF);   // M = 0, Q = 0
    }
    int n = (int)std::lround((altFt + 1000) / 25.0);
    if (n < 1 || n > 2047) return 0;
    return (((unsigned)n >> 5) << 7) | ((((unsigned)n >> 4) & 1) << 5) | 0x10 | ((unsigned)n & 0xF);
}

unsigned encodeAlt12(int altFt, bool gillham) {
    const unsigned a = encodeAc13(altFt, gillham);
    return ((a & 0x1F80) >> 1) | (a & 0x3F);   // remove M
}

Frame encodeAllCall(uint32_t icao, int ca, int ii) {
    Frame f; f.bits = 56;
    setBits(f.b, 1, 5, 11); setBits(f.b, 6, 3, (uint32_t)ca); setBits(f.b, 9, 24, icao);
    setBits(f.b, 33, 24, crcParity(f.b, 56) ^ (uint32_t)(ii & 0x7F));
    return f;
}

static Frame esFrame(uint32_t icao, int ca) {
    Frame f; f.bits = 112;
    setBits(f.b, 1, 5, 17); setBits(f.b, 6, 3, (uint32_t)ca); setBits(f.b, 9, 24, icao);
    return f;
}

Frame encodeIdentification(uint32_t icao, int ca, int tc, int catCode, const std::string& cs) {
    Frame f = esFrame(icao, ca);
    setBits(f.b, 33, 5, (uint32_t)tc); setBits(f.b, 38, 3, (uint32_t)catCode);
    for (int i = 0; i < 8; i++) {
        const int c = i < (int)cs.size() ? charTo6bit(cs[i]) : 32;
        setBits(f.b, 41 + 6 * i, 6, (uint32_t)(c < 0 ? 32 : c));
    }
    finishPi(f);
    return f;
}

static void cprFields(Frame& f, bool odd, int latCpr, int lonCpr) {
    setBits(f.b, 54, 1, odd ? 1 : 0); setBits(f.b, 55, 17, (uint32_t)latCpr); setBits(f.b, 72, 17, (uint32_t)lonCpr);
}

Frame encodeAirbornePosition(uint32_t icao, int ca, int tc, int altFt, bool gillham, bool odd, double lat, double lon) {
    Frame f = esFrame(icao, ca);
    setBits(f.b, 33, 5, (uint32_t)tc);
    setBits(f.b, 41, 12, encodeAlt12(altFt, gillham));
    int la, lo;
    cprEncodeAirborne(lat, lon, odd, la, lo);
    cprFields(f, odd, la, lo);
    finishPi(f);
    return f;
}

Frame encodeGnssPosition(uint32_t icao, int ca, int tc, int heightFt, bool odd, double lat, double lon) {
    Frame f = esFrame(icao, ca);
    setBits(f.b, 33, 5, (uint32_t)tc);
    setBits(f.b, 41, 12, encodeAlt12(heightFt, false));
    int la, lo;
    cprEncodeAirborne(lat, lon, odd, la, lo);
    cprFields(f, odd, la, lo);
    finishPi(f);
    return f;
}

static unsigned movementCode(double kt) {
    if (kt < 0.125) return 1;
    if (kt < 1.0) return 2 + (unsigned)((kt - 0.125) / 0.125);
    if (kt < 2.0) return 9 + (unsigned)((kt - 1.0) / 0.25);
    if (kt < 15.0) return 13 + (unsigned)((kt - 2.0) / 0.5);
    if (kt < 70.0) return 39 + (unsigned)(kt - 15.0);
    if (kt < 100.0) return 94 + (unsigned)((kt - 70.0) / 2.0);
    if (kt < 175.0) return 109 + (unsigned)((kt - 100.0) / 5.0);
    return 124;
}

Frame encodeSurfacePosition(uint32_t icao, int ca, int tc, double kt, double track, bool odd, double lat, double lon) {
    Frame f = esFrame(icao, ca);
    setBits(f.b, 33, 5, (uint32_t)tc);
    setBits(f.b, 38, 7, movementCode(kt));
    setBits(f.b, 45, 1, 1);
    double t = std::fmod(track, 360.0); if (t < 0) t += 360.0;
    setBits(f.b, 46, 7, (uint32_t)std::lround(t * 128.0 / 360.0) & 127);
    int la, lo;
    cprEncodeSurface(lat, lon, odd, la, lo);
    cprFields(f, odd, la, lo);
    finishPi(f);
    return f;
}

static void velocityCommon(Frame& f, int vrateFpm, bool hasDiff, int diffFt) {
    setBits(f.b, 32 + 36, 1, 0);   // vertical rate source: GNSS
    setBits(f.b, 32 + 37, 1, vrateFpm < 0 ? 1 : 0);
    setBits(f.b, 32 + 38, 9, (uint32_t)std::min(std::abs(vrateFpm) / 64 + 1, 511));
    if (hasDiff) {
        setBits(f.b, 32 + 49, 1, diffFt < 0 ? 1 : 0);
        setBits(f.b, 32 + 50, 7, (uint32_t)std::min(std::abs(diffFt) / 25 + 1, 127));
    }
}

Frame encodeVelocity(uint32_t icao, int ca, double speedKt, double trackDeg, int vrateFpm, bool hasDiff, int diffFt) {
    Frame f = esFrame(icao, ca);
    setBits(f.b, 33, 5, 19); setBits(f.b, 38, 3, 1);
    const double r = trackDeg * M_PI / 180.0;
    const int ve = (int)std::lround(speedKt * std::sin(r)), vn = (int)std::lround(speedKt * std::cos(r));
    setBits(f.b, 32 + 14, 1, ve < 0 ? 1 : 0); setBits(f.b, 32 + 15, 10, (uint32_t)std::min(std::abs(ve) + 1, 1023));
    setBits(f.b, 32 + 25, 1, vn < 0 ? 1 : 0); setBits(f.b, 32 + 26, 10, (uint32_t)std::min(std::abs(vn) + 1, 1023));
    velocityCommon(f, vrateFpm, hasDiff, diffFt);
    finishPi(f);
    return f;
}

Frame encodeAirspeed(uint32_t icao, int ca, double speedKt, bool tas, double headingDeg, int vrateFpm) {
    Frame f = esFrame(icao, ca);
    setBits(f.b, 33, 5, 19); setBits(f.b, 38, 3, 3);
    double h = std::fmod(headingDeg, 360.0); if (h < 0) h += 360.0;
    setBits(f.b, 32 + 14, 1, 1);
    setBits(f.b, 32 + 15, 10, (uint32_t)std::lround(h * 1024.0 / 360.0) & 1023);
    setBits(f.b, 32 + 25, 1, tas ? 1 : 0);
    setBits(f.b, 32 + 26, 10, (uint32_t)std::min((int)std::lround(speedKt) + 1, 1023));
    velocityCommon(f, vrateFpm, false, 0);
    finishPi(f);
    return f;
}

Frame encodeAircraftStatus(uint32_t icao, int ca, int emergency, int squawk) {
    Frame f = esFrame(icao, ca);
    setBits(f.b, 33, 5, 28); setBits(f.b, 38, 3, 1); setBits(f.b, 41, 3, (uint32_t)emergency); setBits(f.b, 44, 13, encodeId13(squawk));
    finishPi(f);
    return f;
}

Frame encodeTargetState(uint32_t icao, int ca, int selAltFt, double baroMb, double selHdgDeg, int nacp) {
    Frame f = esFrame(icao, ca);
    setBits(f.b, 33, 5, 29); setBits(f.b, 38, 2, 1);
    if (selAltFt >= 0) setBits(f.b, 42, 11, (uint32_t)std::min(selAltFt / 32 + 1, 2047));
    if (baroMb > 0) setBits(f.b, 53, 9, (uint32_t)std::clamp((int)std::lround((baroMb - 800.0) / 0.8) + 1, 1, 511));
    if (selHdgDeg >= 0) {
        setBits(f.b, 62, 1, 1);
        int h = (int)std::lround(selHdgDeg * 256.0 / 180.0);
        if (h >= 256) h -= 512;
        setBits(f.b, 63, 9, (uint32_t)h & 0x1FF);
    }
    setBits(f.b, 72, 4, (uint32_t)nacp);
    finishPi(f);
    return f;
}

Frame encodeOperationalStatus(uint32_t icao, int ca, int version, int nacp, int sil) {
    Frame f = esFrame(icao, ca);
    setBits(f.b, 33, 5, 31); setBits(f.b, 38, 3, 0);
    setBits(f.b, 73, 3, (uint32_t)version); setBits(f.b, 77, 4, (uint32_t)nacp); setBits(f.b, 83, 2, (uint32_t)sil);
    finishPi(f);
    return f;
}

Frame encodeTisb(const Frame& es, int cf) {
    Frame f = es;
    setBits(f.b, 1, 5, 18); setBits(f.b, 6, 3, (uint32_t)cf);
    finishPi(f);
    return f;
}

Frame encodeAltitudeReply(uint32_t icao, int fs, int altFt, bool gillham) {
    Frame f; f.bits = 56;
    setBits(f.b, 1, 5, 4); setBits(f.b, 6, 3, (uint32_t)fs); setBits(f.b, 20, 13, encodeAc13(altFt, gillham));
    finishAp(f, icao);
    return f;
}

Frame encodeIdentityReply(uint32_t icao, int fs, int squawk) {
    Frame f; f.bits = 56;
    setBits(f.b, 1, 5, 5); setBits(f.b, 6, 3, (uint32_t)fs); setBits(f.b, 20, 13, encodeId13(squawk));
    finishAp(f, icao);
    return f;
}

Frame encodeAirAir(uint32_t icao, bool ground, int altFt, bool longForm) {
    Frame f; f.bits = longForm ? 112 : 56;
    setBits(f.b, 1, 5, longForm ? 16 : 0); setBits(f.b, 6, 1, ground ? 1 : 0); setBits(f.b, 20, 13, encodeAc13(altFt, false));
    finishAp(f, icao);
    return f;
}

Frame encodeCommB(uint32_t icao, bool identity, int fs, int v, const uint8_t mb[7]) {
    Frame f; f.bits = 112;
    setBits(f.b, 1, 5, identity ? 21 : 20); setBits(f.b, 6, 3, (uint32_t)fs);
    setBits(f.b, 20, 13, identity ? encodeId13(v) : encodeAc13(v, false));
    std::memcpy(f.b + 4, mb, 7);
    finishAp(f, icao);
    return f;
}

void mbCallsign(uint8_t mb[7], const std::string& cs) {
    std::memset(mb, 0, 7);
    mb[0] = 0x20;
    for (int i = 0; i < 8; i++) {
        const int c = i < (int)cs.size() ? charTo6bit(cs[i]) : 32;
        setBits(mb, 9 + 6 * i, 6, (uint32_t)(c < 0 ? 32 : c));
    }
}

static void signedField(uint8_t* mb, int statusBit, int first, int nbits, double value, double lsb) {
    setBits(mb, statusBit, 1, 1);
    int v = (int)std::lround(value / lsb);
    const int lim = 1 << (nbits - 1);
    v = std::clamp(v, -lim, lim - 1);
    setBits(mb, first, nbits, (uint32_t)v & ((1u << nbits) - 1));
}

static void unsignedField(uint8_t* mb, int statusBit, int first, int nbits, double value, double lsb) {
    setBits(mb, statusBit, 1, 1);
    const int v = std::clamp((int)std::lround(value / lsb), 0, (1 << nbits) - 1);
    setBits(mb, first, nbits, (uint32_t)v);
}

void mbSelectedVertical(uint8_t mb[7], int mcpFt, int fmsFt, double baroMb) {
    std::memset(mb, 0, 7);
    if (mcpFt >= 0) unsignedField(mb, 1, 2, 12, mcpFt, 16);
    if (fmsFt >= 0) unsignedField(mb, 14, 15, 12, fmsFt, 16);
    if (baroMb > 0) unsignedField(mb, 27, 28, 12, baroMb - 800.0, 0.1);
}

void mbTrackAndTurn(uint8_t mb[7], double roll, double track, double gs, double rate, double tas) {
    std::memset(mb, 0, 7);
    signedField(mb, 1, 2, 10, roll, 45.0 / 256.0);
    double t = std::fmod(track, 360.0); if (t > 180) t -= 360.0; if (t < -180) t += 360.0;
    signedField(mb, 12, 13, 11, t, 90.0 / 512.0);
    unsignedField(mb, 24, 25, 10, gs, 2);
    signedField(mb, 35, 36, 10, rate, 8.0 / 256.0);
    unsignedField(mb, 46, 47, 10, tas, 2);
}

void mbHeadingAndSpeed(uint8_t mb[7], double hdg, double ias, double mach, int baroRate, int inertialRate) {
    std::memset(mb, 0, 7);
    double h = std::fmod(hdg, 360.0); if (h > 180) h -= 360.0; if (h < -180) h += 360.0;
    signedField(mb, 1, 2, 11, h, 90.0 / 512.0);
    unsignedField(mb, 13, 14, 10, ias, 1);
    unsignedField(mb, 24, 25, 10, mach, 0.004);
    signedField(mb, 35, 36, 10, baroRate, 32);
    signedField(mb, 46, 47, 10, inertialRate, 32);
}

void mbDataLinkCap(uint8_t mb[7], int version, bool acas) {
    std::memset(mb, 0, 7);
    mb[0] = 0x10;
    setBits(mb, 17, 7, (uint32_t)version);
    setBits(mb, 25, 1, 1);                 // Mode S specific services
    setBits(mb, 33, 1, 1);                 // aircraft identification capability
    setBits(mb, 36, 1, 1);                 // common usage GICB capability report
    if (acas) setBits(mb, 38, 1, 1);       // ACAS generates resolution advisories
}

void mbGicbCap(uint8_t mb[7], uint32_t mask) {
    std::memset(mb, 0, 7);
    setBits(mb, 1, 24, mask & 0xFFFFFF);
}

void mbResolutionAdvisory(uint8_t mb[7], unsigned ara, unsigned rac, bool rat, bool mte, uint32_t threat) {
    std::memset(mb, 0, 7);
    mb[0] = 0x30;
    setBits(mb, 9, 14, ara); setBits(mb, 23, 4, rac); setBits(mb, 27, 1, rat); setBits(mb, 28, 1, mte);
    setBits(mb, 29, 2, 1); setBits(mb, 31, 24, threat & 0xFFFFFF);
}

static void signMag(uint8_t* mb, int signBit, int first, int n, double v, double lsb) {
    int q = (int)std::lround(v / lsb);
    const int lim = 1 << n;
    q = std::clamp(q, -lim, lim - 1);
    setBits(mb, signBit, 1, q < 0);
    setBits(mb, first, n, (uint32_t)(q & (lim - 1)));
}

void mbMetRoutine(uint8_t mb[7], int fom, double windKt, double windDir, double satC, double hpa, int turb, double hum) {
    std::memset(mb, 0, 7);
    setBits(mb, 1, 4, (uint32_t)std::max(fom, 0));
    if (windKt >= 0) {
        setBits(mb, 5, 1, 1);
        setBits(mb, 6, 9, (uint32_t)std::clamp((int)std::lround(windKt), 0, 511));
        setBits(mb, 15, 9, (uint32_t)((int)std::lround(std::fmod(windDir + 360.0, 360.0) / (180.0 / 256.0)) & 511));
    }
    if (!std::isnan(satC)) signMag(mb, 24, 25, 10, satC, 0.25);   // no status bit: the temperature is always sent
    if (hpa >= 0) unsignedField(mb, 35, 36, 11, hpa, 1);
    if (turb >= 0) { setBits(mb, 47, 1, 1); setBits(mb, 48, 2, (uint32_t)turb); }
    if (hum >= 0) unsignedField(mb, 50, 51, 6, hum, 100.0 / 64.0);
}

void mbMetHazard(uint8_t mb[7], int turb, int shear, int burst, int icing, int wake, double satC, double hpa, double rh) {
    std::memset(mb, 0, 7);
    const int lv[5] = {turb, shear, burst, icing, wake};
    for (int i = 0; i < 5; i++) if (lv[i] >= 0) { setBits(mb, 1 + 3 * i, 1, 1); setBits(mb, 2 + 3 * i, 2, (uint32_t)lv[i]); }
    if (!std::isnan(satC)) { setBits(mb, 16, 1, 1); signMag(mb, 17, 18, 9, satC, 0.25); }
    if (hpa >= 0) unsignedField(mb, 27, 28, 11, hpa, 1);
    if (rh >= 0) unsignedField(mb, 39, 40, 12, rh, 16);
}

} // namespace dect2::adsb
