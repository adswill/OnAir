// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pure logic for the "easy first use" hints (docs/EASE-3.8.md): a next-pass sentence, whether a geostationary satellite is above this
// location's horizon, what a quiet receiver means for a mode, what antenna a mode needs. No ImGui, no App, so tests/app/test_ease.cpp covers it.
// The hints only inform: nothing here blocks a mode, changes a setting or starts a radio.
#pragma once
#include "dect2/specan/geo_look.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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

// ---- E9, E10, E11
namespace ease {

// E9: the band to open on: the highest listed frequency under the path's usable limit (0.85 of the MUF), the lowest band when nothing is usable.
// bands: dial frequencies low to high. Returns the index, -1 when the list is empty.
struct HfBand { const char* name; double mhz; };
inline int pickHfBand(const std::vector<HfBand>& bands, double mufMhz) {
    if (bands.empty()) return -1;
    int best = 0;
    for (int i = 0; i < (int)bands.size(); i++) if (bands[i].mhz <= 0.85 * mufMhz) best = i;
    return best;
}
inline std::vector<HfBand> hfBandsOf(const char* modeName) {
    const std::string m = modeName ? modeName : "";
    if (m == "FT8") return {{"80 m", 3.573}, {"40 m", 7.074}, {"30 m", 10.136}, {"20 m", 14.074}, {"17 m", 18.100}, {"15 m", 21.074}, {"12 m", 24.915}, {"10 m", 28.074}};
    if (m == "FT4") return {{"80 m", 3.575}, {"40 m", 7.0475}, {"30 m", 10.140}, {"20 m", 14.080}, {"17 m", 18.104}, {"15 m", 21.140}, {"12 m", 24.919}, {"10 m", 28.180}};
    if (m == "WSPR") return {{"80 m", 3.5686}, {"40 m", 7.0386}, {"30 m", 10.1387}, {"20 m", 14.0956}, {"17 m", 18.1046}, {"15 m", 21.0946}, {"12 m", 24.9246}, {"10 m", 28.1246}};
    if (m == "JS8Call") return {{"80 m", 3.578}, {"40 m", 7.078}, {"30 m", 10.130}, {"20 m", 14.078}, {"17 m", 18.104}, {"15 m", 21.078}, {"12 m", 24.922}, {"10 m", 28.078}};
    if (m == "PSK31") return {{"80 m", 3.580}, {"40 m", 7.070}, {"30 m", 10.141}, {"20 m", 14.070}, {"17 m", 18.100}, {"15 m", 21.070}, {"10 m", 28.120}};
    if (m == "RTTY") return {{"80 m", 3.590}, {"40 m", 7.045}, {"20 m", 14.085}, {"15 m", 21.085}, {"10 m", 28.085}};
    if (m == "CW") return {{"80 m", 3.560}, {"40 m", 7.030}, {"30 m", 10.116}, {"20 m", 14.060}, {"17 m", 18.086}, {"15 m", 21.060}, {"12 m", 24.906}, {"10 m", 28.060}};
    return {};
}
// "40 m: 20 m is closed now"; empty when the band that was going to be used is usable anyway
inline std::string hfBandSay(const HfBand& chosen, const HfBand& original) {
    if (chosen.mhz >= original.mhz - 0.5) return "";
    return std::string(chosen.name) + ": " + original.name + " is closed now";
}

// E10: for the modes where a quiet screen is normal, what to expect. Null when the mode has no such note.
struct Quiet { const char* mode; const char* text; };
inline const char* quietNote(const char* modeName) {
    static const Quiet k[] = {
        {"DSC", N_("Normal: DSC calls can be many minutes apart.")},
        {"NAVTEX", N_("Normal: NAVTEX is broadcast in scheduled slots, several hours apart per station.")},
        {"Weather fax", N_("Normal: charts follow a broadcast schedule; nothing is sent between them.")},
        {"COSPAS-SARSAT 406 MHz", N_("Normal: 406 MHz beacons are rare, mostly tests and exercises.")},
        {"POCSAG", N_("Normal: pagers can be quiet for minutes between messages.")},
        {"FLEX", N_("Normal: pagers can be quiet for minutes between messages.")},
        {"ISM sensors", N_("Normal: sensors send every 30 s to a few minutes.")},
        {"Sub-GHz sensors", N_("Normal: sensors send every 30 s to a few minutes.")},
        {"ACARS", N_("Normal: aircraft send only when nearby; gaps of minutes are normal.")},
        {"VDL2", N_("Normal: aircraft send only when nearby; gaps of minutes are normal.")},
        {"HFDL", N_("Normal: traffic comes and goes with the band; low frequencies work best at night.")},
        {"SELCAL (aircraft tone call)", N_("Normal: selective calls are sporadic.")},
        {"AIS", N_("Normal: without ships in range there is nothing to hear.")},
        {"Radiosonde", N_("Normal: most sites launch near 11:15 and 23:15 UTC.")},
        {"FLARM", N_("Normal: gliders fly by day and in season.")},
        {"LoRaWAN", N_("Normal: nodes send seconds to hours apart.")},
        {"Meshtastic", N_("Normal: nodes send seconds to hours apart.")},
        {"MeshCore", N_("Normal: nodes send seconds to hours apart.")},
        {"LoRa (raw)", N_("Normal: nodes send seconds to hours apart.")},
        {"Tone decoders (CTCSS, DCS, DTMF, selcall)", N_("Normal: tones are heard only when someone transmits.")},
    };
    if (!modeName) return nullptr;
    for (const Quiet& q : k) if (!std::strcmp(q.mode, modeName)) return q.text;
    return nullptr;
}
inline std::string quietLine(const char* note, double runSec) {
    char b[48];
    std::snprintf(b, sizeof b, " Listening for %ld min.", std::max(0L, std::lround(runSec / 60)));
    return std::string(note) + b;
}

// E11: what antenna the mode needs: from the frequency and a short table of the special cases. Only facts and sums: quarter-wave length from the frequency.
struct AntennaNeed { std::string text; double mhz = 0; };
inline AntennaNeed antennaNeed(const char* modeName, double mhz) {
    const std::string m = modeName ? modeName : "";
    AntennaNeed n; n.mhz = mhz;
    struct Row { const char* mode; const char* text; };
    static const Row rows[] = {
        {"ADS-B", N_("a 1090 MHz collinear or ground plane, outside and high")}, {"UAT", N_("a 978 MHz vertical, outside and high")},
        {"Mode S uplink", N_("a 1030 MHz vertical")}, {"Mode A/C", N_("a 1090 MHz collinear or ground plane")},
        {"GNSS", N_("an active patch antenna with antenna power, open sky")}, {"SBAS", N_("an active patch antenna with antenna power, open sky")},
        {"Meteor-M LRPT", N_("a 137 MHz V-dipole or QFH, right-hand circular")}, {"NOAA APT", N_("a 137 MHz V-dipole or QFH")}, {"Argos", N_("a 466 MHz vertical, open sky")},
        {"QO-100", N_("a 60 cm dish or larger with a Ku LNB")}, {"DVB-S/S2", N_("a dish with a universal LNB")}, {"ISDB-S (BS)", N_("a 45 cm dish with an LNB")},
        {"Inmarsat-C", N_("an L-band patch aimed at the satellite")}, {"Inmarsat Aero", N_("an L-band patch aimed at the satellite")}, {"Inmarsat BGAN", N_("an L-band patch aimed at the satellite")},
        {"Iridium", N_("an L-band patch with open sky")}, {"NFC / RFID 13.56 MHz", N_("a loop within a few centimetres of the reader")},
        {"Time signals", N_("a loop or long wire, far from electronics")}, {"e-Czas (225 kHz, Poland)", N_("a loop or long wire")},
        {"Sferics (lightning)", N_("a long wire or large loop")},
        {"Lightning position (network)", N_("a long wire or large loop")},
        {"GOES HRIT", N_("a dish of 80 cm or larger with a 1.7 GHz LNA")}, {"Elektro-L", N_("a dish of 80 cm or larger with a 1.7 GHz LNA")},
        {"GK-2A LRIT", N_("a dish of 80 cm or larger with a 1.7 GHz LNA")}, {"GK-2A HRIT", N_("a dish of 1.2 m or larger with a 1.7 GHz LNA")},
        {"FY-4 LRIT", N_("a dish of 80 cm or larger with a 1.7 GHz LNA")}, {"FY-2 S-VISSR", N_("a dish of 1.2 m or larger with a 1.7 GHz LNA")},
        {"HRPT / AHRPT", N_("a tracked dish with a 1.7 GHz LNA")}, {"NOAA HRPT", N_("a tracked dish with a 1.7 GHz LNA")}, {"Meteor HRPT", N_("a tracked dish with a 1.7 GHz LNA")}, {"FY-3 AHRPT", N_("a tracked dish with a 1.7 GHz LNA")},
    };
    for (const Row& r : rows) if (m == r.mode) { n.text = r.text; return n; }
    if (mhz <= 0) return n;
    char b[100];
    if (mhz < 30) n.text = N_("a long wire or a loop");
    else {
        const double quarterCm = 7500.0 / mhz;
        if (quarterCm >= 100) std::snprintf(b, sizeof b, N_("a vertical or dipole, quarter wave %.2f m"), quarterCm / 100);
        else std::snprintf(b, sizeof b, N_("a vertical or dipole, quarter wave %.0f cm"), quarterCm);
        n.text = b;
    }
    return n;
}
// The stated antenna (learn::Antenna: 0 whip, 1 dipole, 2 discone, 3 loop, 4 long wire, 5 none): a word when it does not suit this frequency. Empty when it does (or for wide-band dish modes where the row already says what to get).
inline const char* antennaWarn(int stated, double mhz) {
    if (mhz <= 0) return "";
    switch (stated) {
        case 0: return mhz > 1000 ? N_("The small whip is poor above 1 GHz.") : mhz < 30 ? N_("The small whip is poor on short wave.") : "";
        case 1: return mhz > 2000 ? N_("A dipole is poor at this frequency.") : "";
        case 2: return mhz < 25 ? N_("A discone is poor below 25 MHz.") : mhz > 1300 ? N_("A discone is poor above 1.3 GHz.") : "";
        case 3: case 4: return mhz > 30 ? N_("Your HF antenna is poor at this frequency.") : "";
        case 5: return N_("Without an antenna almost nothing is heard.");
        default: return "";
    }
}

}  // namespace ease

// ---- E12, E15
namespace ease {

// E12: a held GNSS fix against the station position. distKm: from the fix to the current position (ignored when none is set)
enum class GnssPos { None, UseIt, Offer };
inline GnssPos gnssPosAction(bool positionSet, double heldSec, double distKm) {
    if (heldSec < 15) return GnssPos::None;
    if (!positionSet) return GnssPos::UseIt;
    return distKm > 5 ? GnssPos::Offer : GnssPos::None;
}

// E15: which of an airport's channels to listen to for an aircraft, by height and distance from the airport (the OurAirports frequency types, best first)
inline std::vector<std::string> listenTypes(int altFt, double distKm) {
    if (altFt < 3000 && distKm < 25) return {"TWR", "APP", "DEP", "CTAF", "GND"};
    if (altFt < 12000 && distKm < 80) return {"APP", "DEP", "ARR", "TWR"};
    return {"CTR", "CENTER", "ACC", "APP", "DEP", "TWR"};
}

}  // namespace ease
