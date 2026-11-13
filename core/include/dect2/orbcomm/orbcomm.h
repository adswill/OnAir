// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Orbcomm subscriber downlink format (137.2 - 137.8 MHz), shared by the receiver, the test transmitter and the tests.
//
// 4800 bit/s SDPSK (each bit turns the phase by +90 degrees for a one, -90 degrees for a zero), root raised cosine (roll-off 0.4).
// The bits of a byte go least significant first. A frame is one second: 600 bytes = 50 packets of 12 bytes; the first packet of a
// frame is the sync packet (65 A8 F9, then the satellite ID). Every packet ends in a Fletcher checksum: both running sums over all
// 12 bytes are zero (mod 256). The packet type is the first byte. The layout follows the public descriptions of the Orbcomm
// downlink (receiver write-ups by the amateur community); the field details of the ephemeris and orbital element packets are this
// decoder's own reading and are documented here.
//
//   0x65 sync         65 A8 F9 | sat ID | frame number | 5 spare | checksum
//   0x1A message      1A | message ID | part (bit 7: last) | 7 characters | checksum
//   0x1B uplink       1B | count | 4 x uint16 channel number (148.000 MHz + n x 2.5 kHz) | checksum
//   0x1C downlink     1C | count | 4 x uint16 channel number (137.000 MHz + n x 2.5 kHz) | checksum
//   0x1D network      1D | gateway | status flags | 7 spare | checksum
//   0x1E fill         1E | 9 bytes | checksum
//   0x1F ephemeris    1F | 72 bits: x, y, z (20 bit signed, 1/64 km, earth fixed), seconds of the hour (12 bit) | checksum
//   0x22 elements     22 | sat ID | inclination, RAAN, argument of latitude (0.01 degree), period (0.01 min), uint16 each | checksum
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace dect2::orbcomm {

constexpr double kSymbolRate = 4800;
constexpr double kRollOff = 0.4;
constexpr int kPktBytes = 12;
constexpr int kFramePkts = 50;
constexpr uint8_t kSync[3] = {0x65, 0xA8, 0xF9};
constexpr double kRasterBaseHz = 137.0e6, kRasterHz = 12.5e3;   // downlink channel raster
constexpr double kEphLsbKm = 1.0 / 64;

enum PktType { TSync = 0, TMessage, TUplink, TDownlink, TEphemeris, TElements, TNetwork, TFill, TOther, kTypeSlots };

inline int typeOf(uint8_t code) {
    switch (code) {
    case 0x65: return TSync;
    case 0x1A: return TMessage;
    case 0x1B: return TUplink;
    case 0x1C: return TDownlink;
    case 0x1F: return TEphemeris;
    case 0x22: return TElements;
    case 0x1D: return TNetwork;
    case 0x1E: return TFill;
    default: return TOther;
    }
}
inline uint8_t codeOf(int type) {
    static const uint8_t c[kTypeSlots] = {0x65, 0x1A, 0x1B, 0x1C, 0x1F, 0x22, 0x1D, 0x1E, 0x00};
    return type >= 0 && type < kTypeSlots ? c[type] : 0;
}
inline const char* typeLabel(int type) {
    static const char* const n[kTypeSlots] = {"sync", "message", "uplink", "downlink", "ephemeris", "elements", "network", "fill", "other"};
    return type >= 0 && type < kTypeSlots ? n[type] : "other";
}

// Fletcher-16 over the whole packet: a good packet leaves both sums at zero
inline bool fletcherOk(const uint8_t* p, int n = kPktBytes) {
    unsigned a = 0, b = 0;
    for (int i = 0; i < n; i++) { a = (a + p[i]) & 255; b = (b + a) & 255; }
    return a == 0 && b == 0;
}
// sets the last two bytes so that fletcherOk holds
inline void fletcherSet(uint8_t* p, int n = kPktBytes) {
    unsigned a = 0, b = 0;
    for (int i = 0; i < n - 2; i++) { a = (a + p[i]) & 255; b = (b + a) & 255; }
    const unsigned x = (512 - ((a + b) & 255)) & 255;
    p[n - 2] = (uint8_t)x;
    p[n - 1] = (uint8_t)b;
}

inline uint16_t u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline void putU16(uint8_t* p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

// ephemeris: bytes 1..9 hold 72 bits, least significant bit first
inline void packEphemeris(uint8_t* p, double xKm, double yKm, double zKm, int secOfHour) {
    uint64_t lo = 0; unsigned hi = 0;   // 72 bits: lo 0..63, hi 64..71
    auto put = [&](int pos, int bits, uint32_t v) {
        for (int i = 0; i < bits; i++) {
            const unsigned bit = (v >> i) & 1u;
            const int q = pos + i;
            if (q < 64) lo |= (uint64_t)bit << q; else hi |= bit << (q - 64);
        }
    };
    auto enc = [](double km) { long v = std::lround(km / kEphLsbKm); v = v < -(1 << 19) ? -(1 << 19) : v > (1 << 19) - 1 ? (1 << 19) - 1 : v; return (uint32_t)v & 0xFFFFFu; };
    put(0, 20, enc(xKm)); put(20, 20, enc(yKm)); put(40, 20, enc(zKm)); put(60, 12, (uint32_t)(secOfHour & 4095));
    for (int i = 0; i < 8; i++) p[1 + i] = (uint8_t)(lo >> (8 * i));
    p[9] = (uint8_t)hi;
}
inline void unpackEphemeris(const uint8_t* p, double& xKm, double& yKm, double& zKm, int& secOfHour) {
    auto bit = [&](int q) { return (p[1 + q / 8] >> (q % 8)) & 1; };
    auto get = [&](int pos, int bits) { uint32_t v = 0; for (int i = 0; i < bits; i++) v |= (uint32_t)bit(pos + i) << i; return v; };
    auto dec = [](uint32_t v) { int32_t s = (int32_t)(v << 12) >> 12; return s * kEphLsbKm; };
    xKm = dec(get(0, 20)); yKm = dec(get(20, 20)); zKm = dec(get(40, 20)); secOfHour = (int)get(60, 12);
}

// earth fixed (km) to WGS84 latitude, longitude (degrees) and height (km)
inline void ecefToGeo(double x, double y, double z, double& lat, double& lon, double& altKm) {
    const double a = 6378.137, f = 1 / 298.257223563, e2 = f * (2 - f);
    const double p = std::hypot(x, y);
    lon = std::atan2(y, x) * 180 / M_PI;
    double phi = std::atan2(z, p * (1 - e2)), h = 0;
    for (int i = 0; i < 6; i++) {
        const double s = std::sin(phi), N = a / std::sqrt(1 - e2 * s * s);
        h = p / std::cos(phi) - N;
        phi = std::atan2(z, p * (1 - e2 * N / (N + h)));
    }
    lat = phi * 180 / M_PI; altKm = h;
}

// the nearest downlink raster channel of an absolute frequency
inline double rasterHz(double hz) { return kRasterBaseHz + std::round((hz - kRasterBaseHz) / kRasterHz) * kRasterHz; }

// one line about a packet, for the packet log
inline std::string describe(const uint8_t* p) {
    char b[160];
    switch (typeOf(p[0])) {
    case TSync: snprintf(b, sizeof b, "sync  satellite %d  frame %d", p[3], p[4]); break;
    case TMessage: {
        char t[8]; for (int i = 0; i < 7; i++) { const uint8_t c = p[3 + i]; t[i] = c >= 32 && c < 127 ? (char)c : '.'; } t[7] = 0;
        snprintf(b, sizeof b, "message %d part %d%s  \"%s\"", p[1], p[2] & 0x7F, p[2] & 0x80 ? " (last)" : "", t);
        break;
    }
    case TUplink: case TDownlink: {
        const bool up = typeOf(p[0]) == TUplink;
        int n = snprintf(b, sizeof b, "%s channels:", up ? "uplink" : "downlink");
        for (int i = 0; i < 4 && i < p[1]; i++) n += snprintf(b + n, sizeof b - n, " %.4f", (up ? 148.0 : 137.0) + u16(p + 2 + 2 * i) * 0.0025);
        snprintf(b + n, sizeof b - n, " MHz");
        break;
    }
    case TEphemeris: {
        double x, y, z, la, lo, h; int s; unpackEphemeris(p, x, y, z, s); ecefToGeo(x, y, z, la, lo, h);
        snprintf(b, sizeof b, "ephemeris  %.2f %.2f  %.0f km  (x %.0f y %.0f z %.0f km, %d s)", la, lo, h, x, y, z, s);
        break;
    }
    case TElements: snprintf(b, sizeof b, "elements  satellite %d  incl %.2f  RAAN %.2f  arg lat %.2f  period %.2f min", p[1], u16(p + 2) / 100.0, u16(p + 4) / 100.0, u16(p + 6) / 100.0, u16(p + 8) / 100.0); break;
    case TNetwork: snprintf(b, sizeof b, "network control  gateway %d  status %02X", p[1], p[2]); break;
    case TFill: snprintf(b, sizeof b, "fill"); break;
    default: snprintf(b, sizeof b, "type %02X  %02X %02X %02X %02X %02X %02X %02X %02X %02X", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9]); break;
    }
    return b;
}

} // namespace dect2::orbcomm
