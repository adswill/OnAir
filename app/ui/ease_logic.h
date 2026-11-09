// Pure logic for the "easy first use" hints (docs/EASE-3.8.md): a next-pass sentence, whether a geostationary satellite is above this
// location's horizon, what a quiet receiver means for a mode, what antenna a mode needs. No ImGui, no App, so tests/app/test_ease.cpp covers it.
// The hints only inform: nothing here blocks a mode, changes a setting or starts a radio.
#pragma once
#include "dect2/specan/geo_look.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#ifndef N_
#define N_(s) s
#endif

namespace ease {

// ---- E3: the next pass in one sentence. secToAos may be negative (the pass is under way); times are text so the caller keeps its own clock format.
inline std::string durationText(double sec) {
    if (sec < 90) return "1 min";
    char b[32];
    const long m = std::lround(sec / 60);
    if (m < 90) std::snprintf(b, sizeof b, "%ld min", m);
    else std::snprintf(b, sizeof b, "%ld h %02ld min", m / 60, m % 60);
    return b;
}
inline std::string passLine(const std::string& name, const std::string& aosText, double peakDeg, double secToAos, double secToLos) {
    char b[96];
    if (secToAos <= 0) {
        std::snprintf(b, sizeof b, ", peak %.0f deg, sets in %s", peakDeg, durationText(std::max(0.0, secToLos)).c_str());
        return "Overhead now: " + name + b;
    }
    std::snprintf(b, sizeof b, ", %.0f deg, in %s", peakDeg, durationText(secToAos).c_str());
    return "Next pass: " + name + " at " + aosText + b;
}

// ---- E4: can this geostationary satellite be seen from here
struct GeoSat { const char* name; double lonE; };
struct GeoView { const char* name; double azDeg, elDeg, skewDeg; bool above; };

// The LNB rotation for linear polarisation, degrees: positive clockwise seen from behind the dish, for a satellite east of south in the northern hemisphere negative
inline double lnbSkewDeg(double latDeg, double lonDeg, double satLonE) {
    constexpr double d2r = M_PI / 180;
    double d = std::fmod(satLonE - lonDeg, 360.0);
    if (d > 180) d -= 360;
    if (d < -180) d += 360;
    if (std::fabs(latDeg) < 0.5) return 90.0;
    return -std::atan(std::sin(d * d2r) / std::tan(latDeg * d2r)) / d2r;
}
inline GeoView geoView(double latDeg, double lonDeg, const GeoSat& s) {
    const dect2::GeoLookAngle l = dect2::geoLookAngle(latDeg, lonDeg, s.lonE);
    return {s.name, l.azimuthDeg, l.elevationDeg, lnbSkewDeg(latDeg, lonDeg, s.lonE), l.elevationDeg > 0};
}
inline const char* compassName(double azDeg) {
    static const char* const n[8] = {N_("north"), N_("north-east"), N_("east"), N_("south-east"), N_("south"), N_("south-west"), N_("west"), N_("north-west")};
    return n[(int)std::floor(std::fmod(azDeg + 22.5 + 360, 360.0) / 45) % 8];
}
// The satellites a mode listens to (empty: not a geostationary mode). Longitudes from the modes' own text, inmsat_satid and the rotator's slot list;
// operators do move satellites, so the card says what it assumes.
inline std::vector<GeoSat> geoSatsOf(int family) {
    switch (family) {
        case 60: return {{"GOES-19", -75.2}, {"GOES-18", -137.0}};
        case 80: case 265: return {{"Elektro-L 76E", 76.0}, {"Elektro-L 14.5W", -14.5}, {"Elektro-L 165.8E", 165.8}};
        case 230: case 231: return {{"GK-2A", 128.2}};
        case 232: case 233: return {{"FY-4A", 104.7}, {"FY-4B", 105.0}};
        case 234: return {{"FY-2G", 105.0}, {"FY-2H", 79.0}};
        case 53: return {{"QO-100", 25.9}};
        case 154: return {{"BS (Japan)", 110.0}};
        case 6: case 144: return {{"Astra 19.2E", 19.2}, {"Hotbird 13E", 13.0}, {"Eutelsat 16E", 16.0}, {"Astra 23.5E", 23.5}, {"Astra 28.2E", 28.2}, {"Eutelsat 9E", 9.0}, {"Thor 0.8W", -0.8}, {"Galaxy 97W", -97.0}};
        case 17: case 18: case 208: case 245: return {{"Alphasat 25E", 25.0}, {"Inmarsat-4 F3 98W", -98.0}, {"Inmarsat-3 F5 54W", -54.0}, {"Inmarsat-6 F1 83.5E", 83.5}};
        default: return {};
    }
}
inline bool geoNeedsSkew(int family) { return family == 6 || family == 144 || family == 53 || family == 154; }    // a Ku band LNB with linear polarisation
// "<name>: south-east 137 deg, 27.6 deg up" or "<name>: below your horizon (-14.5 deg)"
inline std::string geoLine(const GeoView& v, bool withSkew) {
    char b[128];
    if (!v.above) { std::snprintf(b, sizeof b, ": below your horizon (%.1f deg)", v.elDeg); return std::string(v.name) + b; }
    if (withSkew) std::snprintf(b, sizeof b, ": %s %.0f deg, %.1f deg up, LNB skew %+.0f deg", compassName(v.azDeg), v.azDeg, v.elDeg, v.skewDeg);
    else std::snprintf(b, sizeof b, ": %s %.0f deg, %.1f deg up", compassName(v.azDeg), v.azDeg, v.elDeg);
    return std::string(v.name) + b;
}
// Index of the highest satellite of the list, -1 when empty
inline int geoBest(const std::vector<GeoView>& vs) {
    int b = -1;
    for (int i = 0; i < (int)vs.size(); i++) if (b < 0 || vs[i].elDeg > vs[b].elDeg) b = i;
    return b;
}

}  // namespace ease

// ---- E5, E6, E7: what the first Start of a mode does by itself
namespace ease {

// Happens once and is remembered; never when the user already has a list, has moved off the default frequency, is recording, or the radio is not a real one
struct FirstIn { bool done = false, realRadio = false, emptyList = false, atDefaultFreq = false, recording = false, transmitting = false, busy = false; };
inline bool firstScanWanted(const FirstIn& i) { return !i.done && i.realRadio && i.emptyList && i.atDefaultFreq && !i.recording && !i.transmitting && !i.busy; }

// A TV service to play first: the lowest logical channel number of the television services (radio services only when there is no television), numbered ones before unnumbered; -1 when none
struct SvcPick { int id, lcn; bool radio; };
inline int pickFirstService(const std::vector<SvcPick>& v) {
    int best = -1;
    auto key = [](const SvcPick& s) { return std::pair<int, int>((s.radio ? 2 : 0) + (s.lcn > 0 ? 0 : 1), s.lcn > 0 ? s.lcn : s.id); };
    for (int i = 0; i < (int)v.size(); i++) if (v[i].id >= 0 && (best < 0 || key(v[i]) < key(v[best]))) best = i;
    return best;
}

// Index of the largest value, -1 when empty (the strongest station or multiplex of a scan)
inline int strongestIdx(const std::vector<float>& snr) {
    int b = -1;
    for (int i = 0; i < (int)snr.size(); i++) if (b < 0 || snr[i] > snr[b]) b = i;
    return b;
}

}  // namespace ease
