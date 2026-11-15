// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The selected radiosonde as a GPS stream (4.2): NMEA 0183 $GPGGA and $GPRMC sentences, so a map program that takes a GPS over a serial
// port (a virtual COM port, a pty, TCP or UDP) can follow the balloon. Pure text building here (no sockets, no serial port); the sender is
// sonde_nmea_out.h. Sentence layouts after NMEA 0183: GGA time, lat, N/S, lon, E/W, quality, satellites, HDOP, altitude, M, geoid, M,
// DGPS age, DGPS station; RMC time, status, lat, N/S, lon, E/W, speed (knots), course (true), date, magnetic variation (blank), mode.
#pragma once
#include "dect2/sonde/sonde_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <string>

namespace dect2 {
namespace sondenmea {

constexpr double kKnotsPerMs = 3600.0 / 1852.0;   // 1 m/s = 1.943844 knots

// The XOR of every character between '$' and '*', as two upper-case hex digits.
inline std::string checksum(const std::string& body) {
    unsigned c = 0;
    for (unsigned char ch : body) c ^= ch;
    char b[4]; std::snprintf(b, sizeof b, "%02X", c & 0xFF);
    return b;
}
// "$" + body + "*" + checksum + CR LF
inline std::string sentence(const std::string& body) { return "$" + body + "*" + checksum(body) + "\r\n"; }

inline double knots(double metresPerSecond) { return metresPerSecond * kKnotsPerMs; }

// ddmm.mmmm,N (latitude, two degree digits) or dddmm.mmmm,E (longitude, three); rounded to 0.0001 minute with the carry into the degrees
inline std::string degMin(double deg, bool isLat) {
    const bool neg = deg < 0;
    const long long t = std::llround(std::fabs(deg) * 60.0 * 10000.0);   // ten-thousandths of a minute
    const long long d = t / 600000, m = t % 600000;
    char b[40];
    std::snprintf(b, sizeof b, isLat ? "%02lld%02lld.%04lld,%c" : "%03lld%02lld.%04lld,%c", d, m / 10000, m % 10000, isLat ? (neg ? 'S' : 'N') : (neg ? 'W' : 'E'));
    return b;
}

inline std::string timeField(double unixT) {   // hhmmss.ss (UTC)
    const double f = std::floor(unixT);
    const time_t tt = (time_t)f;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[24]; std::snprintf(b, sizeof b, "%02d%02d%05.2f", g.tm_hour, g.tm_min, g.tm_sec + (unixT - f));
    return b;
}
inline std::string dateField(double unixT) {   // ddmmyy
    const time_t tt = (time_t)std::floor(unixT);
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[16]; std::snprintf(b, sizeof b, "%02d%02d%02d", g.tm_mday, g.tm_mon + 1, g.tm_year % 100);
    return b;
}

// True when the sonde has a position worth sending: a GPS position, a frame in the last maxAgeS seconds, and fewer than 4 satellites is not
// a fix (unknown satellites are accepted).
inline bool usable(const SondeInfo& s, double maxAgeS = 15) {
    return s.hasPos && std::isfinite(s.lat) && std::isfinite(s.lon) && std::fabs(s.lat) <= 90 && std::fabs(s.lon) <= 180 && !(s.lat == 0 && s.lon == 0) &&
           std::isfinite(s.altM) && s.lastHeardS <= maxAgeS && !(s.sats >= 0 && s.sats < 4);
}

// The time of the position: the sonde's own clock when it sends one, else the computer's (wallUtc).
inline double positionTime(const SondeInfo& s, double wallUtc) { return s.hasTime && s.unixTime > 946684800 ? s.unixTime : wallUtc; }

inline std::string gga(const SondeInfo& s, double wallUtc) {
    char b[160];
    const std::string sats = s.sats >= 0 ? std::to_string(std::min(s.sats, 99)) : std::string();
    std::snprintf(b, sizeof b, "GPGGA,%s,%s,%s,1,%s,1.0,%.1f,M,,M,,", timeField(positionTime(s, wallUtc)).c_str(), degMin(s.lat, true).c_str(), degMin(s.lon, false).c_str(), sats.c_str(), s.altM);
    return sentence(b);
}

inline std::string rmc(const SondeInfo& s, double wallUtc) {
    const double t = positionTime(s, wallUtc);
    double hd = 0, kn = 0;
    if (s.hasVel && std::isfinite(s.hSpeed) && std::isfinite(s.headingDeg)) { kn = knots(std::max(0.0, s.hSpeed)); hd = std::fmod(s.headingDeg, 360.0); if (hd < 0) hd += 360; }
    char b[160];
    std::snprintf(b, sizeof b, "GPRMC,%s,A,%s,%s,%.1f,%.1f,%s,,,A", timeField(t).c_str(), degMin(s.lat, true).c_str(), degMin(s.lon, false).c_str(), kn, hd, dateField(t).c_str());
    return sentence(b);
}

// One second's output: GGA then RMC, "" when the sonde has nothing to send.
inline std::string sentences(const SondeInfo& s, double wallUtc, double maxAgeS = 15) {
    if (!usable(s, maxAgeS)) return {};
    return gga(s, wallUtc) + rmc(s, wallUtc);
}

}  // namespace sondenmea
}  // namespace dect2
