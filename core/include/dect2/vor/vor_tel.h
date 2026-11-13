// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VOR / ILS receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace dect2 {

// What the receiver measures: decided from the setting (VOR or ILS entry) and, for ILS, from the dial frequency
enum class NavFunction : int { None = 0, Vor = 1, Localizer = 2, Glideslope = 3, Marker = 4 };

struct VorTelemetry {
    uint64_t seq = 0;            // grows with every report
    int state = 0;               // 0 no carrier, 1 carrier (no navigation signal measured), 2 measuring (radial or DDM valid)
    double cfoHz = 0;            // carrier offset from the dial frequency as measured
    float snrDb = 0;             // carrier to noise in 25 kHz
    float levelDb = -200;        // power of the whole input, dBFS
    bool dataValid = false;      // radial (VOR) or DDM (ILS) valid
    uint64_t blocksOk = 0;       // measurements (one every 0.25 s)
    uint64_t blocksBad = 0;      // blocks with a carrier but no navigation signal
    // set-up
    NavFunction function = NavFunction::None;
    double timeSec = 0;          // signal seconds since the start
    double inputRate = 0;
    double dialHz = 0;           // the dial frequency (0: not known, a file)
    double demodRate = 0;        // rate of the envelope the tones are measured from
    // VOR
    bool vorValid = false;
    double radialDeg = 0;        // magnetic bearing FROM the station, smoothed (about 2 s), 0 .. 360
    double radialRawDeg = 0;     // the last 0.5 s measurement
    double radialStdDeg = 0;     // scatter of the 0.5 s measurements (circular standard deviation, last 5 s)
    float am30 = 0;              // 30 Hz AM depth (0 .. 1; 0.30 nominal)
    float sub9960 = 0;           // 9960 Hz subcarrier AM depth (0.30 nominal)
    float fmDevHz = 0;           // the subcarrier's 30 Hz FM deviation (480 Hz nominal)
    float subcarrierHz = 0;      // the subcarrier's measured frequency (9960 Hz +- 1 %)
    float tone30Hz = 0;          // the 30 Hz frequency as measured
    // ILS
    bool ilsValid = false;
    double ddm = 0;              // m90 - m150: localizer > 0 left of the centreline (fly right), glide slope > 0 above the path (fly down)
    double sdm = 0;              // m90 + m150 (0.40 localizer, 0.80 glide slope)
    double m90 = 0, m150 = 0;
    double ddmStd = 0;           // scatter of the 0.5 s measurements
    int marker = 0;              // marker beacon: 0 none, 1 outer (400 Hz), 2 middle (1300 Hz), 3 inner (3000 Hz)
    bool markerOn = false;       // its tone is keyed now
    float markerDepth = 0;
    int markerKeying = 0;        // the keying heard: 0 not yet, 1 dashes (outer pattern), 2 dot-dash (middle), 3 dots (inner)
    bool markerKeyingOk = false; // the keying is the one of the tone (marker 1 / 2 / 3 keyed as 1 / 2 / 3)
    // identification (1020 Hz Morse)
    std::string ident;           // the last identification decoded twice the same (empty: none yet)
    std::string identLast;       // the last word decoded (maybe not confirmed)
    std::string morse;           // the dots and dashes of the letter being keyed
    bool identKey = false;       // the tone is on now
    float identDepth = 0;        // 1020 Hz AM depth
    float identWpm = 0;          // speed of the keying, words per minute
    uint64_t identCount = 0;     // words decoded
};

// Bearing helpers: the radial is FROM the station; the bearing TO the station is the reciprocal
inline double navWrap360(double d) { d = std::fmod(d, 360.0); return d < 0 ? d + 360 : d; }
inline double vorToBearing(double radialDeg) { return navWrap360(radialDeg + 180); }

// International Morse for the identifications: letters and digits, '.' and '-' (nullptr for other characters)
inline const char* navMorse(char c) {
    static const char* L[26] = {".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--", "-.", "---", ".--.", "--.-", ".-.",
                                "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--.."};
    static const char* D[10] = {"-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----."};
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c >= 'A' && c <= 'Z') return L[c - 'A'];
    if (c >= '0' && c <= '9') return D[c - '0'];
    return nullptr;
}

inline const char* navFunctionName(NavFunction f) {
    switch (f) {
    case NavFunction::Vor: return "VOR";
    case NavFunction::Localizer: return "localizer";
    case NavFunction::Glideslope: return "glide slope";
    case NavFunction::Marker: return "marker";
    default: return "none";
    }
}

inline std::string vorSummary(const VorTelemetry& t) {
    char b[200];
    const std::string id = t.ident.empty() ? std::string("") : " " + t.ident;
    if (t.state == 0) return "no carrier";
    if (t.function == NavFunction::Vor) {
        if (!t.vorValid) snprintf(b, sizeof b, "VOR%s: carrier, no 30 Hz signals", id.c_str());
        else snprintf(b, sizeof b, "VOR%s radial %05.1f, to %05.1f", id.c_str(), t.radialDeg, vorToBearing(t.radialDeg));
    } else if (t.function == NavFunction::Marker) {
        static const char* nm[] = {"no marker", "outer marker", "middle marker", "inner marker"};
        static const char* kk[] = {"", ", dashes", ", dot-dash", ", dots"};
        snprintf(b, sizeof b, "%s%s%s", nm[t.marker & 3], kk[t.markerKeying & 3], t.markerKeying && !t.markerKeyingOk ? " (keying does not match the tone)" : "");
    } else {
        if (!t.ilsValid) snprintf(b, sizeof b, "%s%s: carrier, no 90 / 150 Hz", navFunctionName(t.function), id.c_str());
        else snprintf(b, sizeof b, "%s%s DDM %+.3f", navFunctionName(t.function), id.c_str(), t.ddm);
    }
    return b;
}

} // namespace dect2
