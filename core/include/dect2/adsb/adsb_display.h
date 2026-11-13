// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#pragma once
// ADS-B display options: the altitude colour scale, the track width and when a silent aircraft leaves the table and the map.
// Pure functions, so they are tested without the GUI (tests/adsb/test_adsb_display.cpp). The app keeps the values in prefs under the keys below.
#include <algorithm>
#include <cstdint>

namespace dect2::adsbd {

inline constexpr const char* kPrefAltColour = "adsbAltColour";   // bool, default true
inline constexpr const char* kPrefTrackWidth = "adsbTrackWidth"; // int px, 1 - 6, default 2
inline constexpr const char* kPrefDropSec = "adsbDropSec";       // int s, 0 = never, default 60
inline constexpr const char* kPrefLabels = "adsbLabels";         // bool, default true: the label boxes on the map (#85: off in crowded areas, the selected aircraft keeps its own)

inline constexpr const char* kPrefLabelScale = "adsbLabelScale";   // int %, 70 - 250, default 100: the size of the text in the label boxes (#368)
inline constexpr const char* kPrefLabelFields = "adsbLabelFields"; // bit mask, default 7: 1 altitude and speed, 2 squawk and category, 4 registration and operator

inline constexpr int kDefaultLabelScale = 100, kMinLabelScale = 70, kMaxLabelScale = 250, kAllLabelFields = 7;
inline constexpr int kDefaultTrackWidth = 2, kMinTrackWidth = 1, kMaxTrackWidth = 6;
inline constexpr int kDefaultDropSec = 60, kMinDropSec = 5, kMaxDropSec = 3600;

struct Options {
    bool altColour = true;
    int trackWidth = kDefaultTrackWidth;
    int dropSec = kDefaultDropSec;   // 0: never
    bool labels = true;              // label boxes beside the aircraft on the map (the selected one always has its box)
    int labelScale = kDefaultLabelScale;
    int labelFields = kAllLabelFields;
};

inline int clampTrackWidth(int w) { return std::max(kMinTrackWidth, std::min(kMaxTrackWidth, w)); }
inline int clampLabelScale(int p) { return std::max(kMinLabelScale, std::min(kMaxLabelScale, p)); }
inline int clampDropSec(int s) { return s <= 0 ? 0 : std::max(kMinDropSec, std::min(kMaxDropSec, s)); }

struct Rgb { uint8_t r, g, b; };

// Low is warm, high is cool: orange at 0 ft, green at 10000, teal at 20000, blue at 30000, violet at 40000 ft and above.
// On the ground: grey. No altitude known: the colour of 15000 ft's neighbourhood (a green-teal), so it is not mistaken for a low aircraft.
inline Rgb altRgb(bool hasAlt, int altFt, bool ground) {
    static const float stops[5][3] = {{0.95f, 0.50f, 0.20f}, {0.55f, 0.85f, 0.40f}, {0.30f, 0.85f, 0.75f}, {0.35f, 0.60f, 0.95f}, {0.72f, 0.52f, 0.95f}};
    if (ground) return {140, 144, 148};
    const float v = hasAlt ? std::max(0.f, std::min(3.999f, altFt / 10000.f)) : 1.5f;
    const int i = (int)v;
    const float f = v - i;
    auto c = [&](int k) { return (uint8_t)(255 * (stops[i][k] + (stops[i + 1][k] - stops[i][k]) * f)); };
    return {c(0), c(1), c(2)};
}

// Has an aircraft that was last heard `ageSec` ago to leave the table and the map? dropSec 0 means never.
inline bool expired(double ageSec, int dropSec) { return dropSec > 0 && ageSec > dropSec; }

// What the tracker is given as its expiry: "never" is a very long time (the table is bounded anyway).
inline double trackerDropSec(int dropSec) { return dropSec > 0 ? (double)dropSec : 1e9; }

// The selected aircraft left the table (the tracker dropped it): drop the selection too, so its readout panel (side pane, full screen map)
// closes instead of staying open and empty (#235). `present`: the selection is in the newest aircraft list. A short grace (graceSec) rides
// over a list that is briefly rebuilt without it. Returns true when the selection should be cleared now; `missingSince` is the caller's state.
inline bool selectionGone(bool haveSelection, bool present, double nowSec, double& missingSince, double graceSec = 1.0) {
    if (!haveSelection || present) { missingSince = -1; return false; }
    if (missingSince < 0) { missingSince = nowSec; return false; }
    if (nowSec - missingSince < graceSec) return false;
    missingSince = -1;
    return true;
}

} // namespace dect2::adsbd
