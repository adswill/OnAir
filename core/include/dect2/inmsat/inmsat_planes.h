// Aircraft list logic of the Inmarsat all-carriers mode, kept out of the screen so it can be tested: merging the aircraft of several windows,
// how old a position is and how far it fades, names, search, sorting, whether a trail segment crosses the date line.
// Positions come from every source the Aero receiver decodes (ADS-C reports, ARINC 702 POS reports, label 16 and 20 reports, see aero_pos.h);
// an aircraft is one row whatever it was heard on. Reports come every few minutes, so ages are in minutes, not seconds.
#pragma once
#include "dect2/aero/aero_tel.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2::inmsat {

// One row per aircraft: the ICAO address when the log-on or a report gave it, else the registration.
inline std::string planeKey(const AeroAircraft& a) {
    char b[16];
    if (a.aesId) { std::snprintf(b, sizeof b, "A%06X", a.aesId); return b; }
    if (!a.registration.empty()) return "R" + a.registration;
    return "?" + a.flight;
}

// Fold `o` (the same aircraft seen by another window, or earlier) into `into`: the newest contact wins, the newest position wins, names are
// filled in, message counts add. The trail is the longer of the two (they cover the same flight).
inline void mergeAircraft(AeroAircraft& into, const AeroAircraft& o) {
    const uint64_t total = into.messages + o.messages;
    const bool oNewer = o.lastWall > into.lastWall;
    const bool takePos = o.hasPos && (!into.hasPos || o.posWall > into.posWall);
    AeroAircraft keep = oNewer ? o : into;
    const AeroAircraft& other = oNewer ? into : o;
    if (keep.registration.empty()) keep.registration = other.registration;
    if (keep.flight.empty()) keep.flight = other.flight;
    if (!keep.icao) keep.icao = other.icao;
    const AeroAircraft& pp = takePos ? o : into;   // the holder of the newest position
    if (pp.hasPos) {
        keep.hasPos = true; keep.lat = pp.lat; keep.lon = pp.lon;
        keep.hasAlt = pp.hasAlt; keep.altFt = pp.altFt;
        keep.hasTrack = pp.hasTrack; keep.trackDeg = pp.trackDeg;
        keep.hasSpeed = pp.hasSpeed; keep.speedKt = pp.speedKt;
        keep.posSource = pp.posSource; keep.posKind = pp.posKind; keep.posTime = pp.posTime; keep.posWall = pp.posWall;
        keep.reportSecPastHour = pp.reportSecPastHour; keep.reportSecOfDay = pp.reportSecOfDay; keep.route = pp.route;
    } else if (other.hasPos) {
        const AeroAircraft& p = other;
        keep.hasPos = true; keep.lat = p.lat; keep.lon = p.lon; keep.hasAlt = p.hasAlt; keep.altFt = p.altFt;
        keep.hasTrack = p.hasTrack; keep.trackDeg = p.trackDeg; keep.hasSpeed = p.hasSpeed; keep.speedKt = p.speedKt;
        keep.posSource = p.posSource; keep.posKind = p.posKind; keep.posTime = p.posTime; keep.posWall = p.posWall; keep.route = p.route;
    }
    if (o.track.size() > keep.track.size() || (takePos && o.track.size() >= keep.track.size())) keep.track = o.track;
    else if (other.track.size() > keep.track.size()) keep.track = other.track;
    keep.positions = std::max(into.positions, o.positions);
    keep.messages = total;
    into = keep;
}

inline const char* planeSourceName(int posSource) { return posSource == 1 ? "ADS-C" : posSource == 2 ? "ACARS text" : "-"; }

inline std::string planeName(const AeroAircraft& a) {
    if (!a.flight.empty()) return a.flight;
    if (!a.registration.empty()) return a.registration;
    char b[16];
    if (a.aesId) { std::snprintf(b, sizeof b, "%06X", a.aesId); return b; }
    if (a.icao) { std::snprintf(b, sizeof b, "%06X", a.icao); return b; }
    return "?";
}

inline std::string planeIcao(const AeroAircraft& a) {
    const unsigned v = a.aesId ? a.aesId : a.icao;
    if (!v) return "";
    char b[16];
    std::snprintf(b, sizeof b, "%06X", v);
    return b;
}

// seconds since the last position, -1 when there is none or the clock is not known
inline double posAgeSec(const AeroAircraft& a, int64_t nowWall) {
    if (!a.hasPos || a.posWall <= 0 || nowWall <= 0) return -1;
    return std::max<double>(0, (double)(nowWall - a.posWall));
}

// How solid a plane is drawn: full for 5 minutes (the usual gap between ADS-C reports is 3 to 15 min on a default contract), then it fades to a
// floor of 0.25 at 40 minutes and stays there for as long as the aircraft is listed; an unknown age is the floor.
inline float posFade(double ageSec) {
    if (ageSec < 0) return 0.25f;
    if (ageSec <= 300) return 1.f;
    if (ageSec >= 2400) return 0.25f;
    return (float)(1.0 - 0.75 * (ageSec - 300) / 2100.0);
}

inline std::string ageText(double sec) {
    if (sec < 0) return "-";
    char b[24];
    if (sec < 90) std::snprintf(b, sizeof b, "%.0f s", sec);
    else if (sec < 5400) std::snprintf(b, sizeof b, "%.0f min", sec / 60);
    else std::snprintf(b, sizeof b, "%.1f h", sec / 3600);
    return b;
}

inline int placedCount(const std::vector<AeroAircraft>& v) {
    int n = 0;
    for (const auto& a : v) n += a.hasPos;
    return n;
}

inline bool containsNoCase(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto low = [](std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; };
    return low(hay).find(low(needle)) != std::string::npos;
}

inline bool planeMatches(const AeroAircraft& a, const std::string& needle) {
    if (needle.empty()) return true;
    return containsNoCase(a.flight + " " + a.registration + " " + planeIcao(a) + " " + a.posKind + " " + a.lastLabel, needle);
}

enum class PlaneSort { Name, Age, Altitude, Speed, Messages };

// indices into v of the aircraft matching `needle`, in the order asked (aircraft with no value for the key go last either way)
inline std::vector<size_t> sortedPlanes(const std::vector<AeroAircraft>& v, PlaneSort key, bool descending, const std::string& needle, int64_t nowWall) {
    std::vector<size_t> idx;
    for (size_t i = 0; i < v.size(); i++) if (planeMatches(v[i], needle)) idx.push_back(i);
    auto has = [&](const AeroAircraft& a) {
        switch (key) {
            case PlaneSort::Age: return a.hasPos;
            case PlaneSort::Altitude: return a.hasPos && a.hasAlt;
            case PlaneSort::Speed: return a.hasSpeed;
            default: return true;
        }
    };
    auto val = [&](const AeroAircraft& a) -> double {
        switch (key) {
            case PlaneSort::Age: return posAgeSec(a, nowWall);
            case PlaneSort::Altitude: return a.altFt;
            case PlaneSort::Speed: return a.speedKt;
            case PlaneSort::Messages: return (double)a.messages;
            default: return 0;
        }
    };
    std::stable_sort(idx.begin(), idx.end(), [&](size_t x, size_t y) {
        const AeroAircraft &a = v[x], &b = v[y];
        const bool ha = has(a), hb = has(b);
        if (ha != hb) return ha;
        if (key == PlaneSort::Name) {
            const std::string na = planeName(a), nb = planeName(b);
            return descending ? na > nb : na < nb;
        }
        const double va = val(a), vb = val(b);
        if (va == vb) return false;
        return descending ? va > vb : va < vb;
    });
    return idx;
}

// A trail segment between two positions that jump more than half the world in longitude crossed the date line: it is not drawn.
inline bool segmentCrossesDateLine(double lon0, double lon1) { return std::fabs(lon1 - lon0) > 180.0; }

// The messages of one aircraft (by ICAO address, else registration), newest last; at most `max` of the newest.
inline std::vector<const AeroMessage*> planeHistory(const std::vector<AeroMessage>& all, const AeroAircraft& a, size_t max) {
    std::vector<const AeroMessage*> out;
    for (const auto& m : all) {
        const bool same = (a.aesId && m.aesId == a.aesId) || (!a.aesId && !a.registration.empty() && m.registration == a.registration);
        if (same) out.push_back(&m);
    }
    if (out.size() > max) out.erase(out.begin(), out.end() - (std::ptrdiff_t)max);
    return out;
}

} // namespace dect2::inmsat
