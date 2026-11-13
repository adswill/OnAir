// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/ham/ham_voice.h"
#include "dect2/geo/maidenhead.h"
#include <cmath>

namespace dect2::ham {

namespace {
constexpr unsigned R1 = 1, R2 = 2, R3 = 4, ALL = 7;
constexpr double SSB = 2700, FM = 12500;
constexpr VoiceMode L = VoiceMode::Lsb, U = VoiceMode::Usb, N = VoiceMode::Nfm;
// MHz in the table. The edges are the IARU band plans' phone segments (HF Region 1 2.7 kHz bandwidth plan, the Region 2 and Region 3 plans,
// the IARU Region 1 VHF/UHF handbook calling frequencies); national plans can narrow them, which is why the app calls it a receive aid.
const struct Row { const char* band; const char* btn; double lo, hi, tune; VoiceMode m; const char* label; unsigned r; } kRows[] = {
    {"160m", "160m", 1.843, 2.000, 1.900, L, "SSB phone", ALL},
    {"80m", "80m", 3.600, 3.800, 3.700, L, "SSB phone", R1 | R3},
    {"80m", "80m", 3.600, 4.000, 3.850, L, "SSB phone", R2},
    {"60m", "60m", 5.3515, 5.3665, 5.3585, U, "SSB phone (upper sideband by convention)", R1 | R3},
    {"60m", "60m", 5.3305, 5.4035, 5.3570, U, "SSB channels (dial 5.3305, 5.3465, 5.3570, 5.3715, 5.4035)", R2},
    {"40m", "40m", 7.060, 7.200, 7.100, L, "SSB phone", R1 | R3},
    {"40m", "40m", 7.125, 7.300, 7.200, L, "SSB phone", R2},
    {"20m", "20m", 14.112, 14.350, 14.200, U, "SSB phone", R1 | R3},
    {"20m", "20m", 14.150, 14.350, 14.250, U, "SSB phone", R2},
    {"17m", "17m", 18.111, 18.168, 18.130, U, "SSB phone", ALL},
    {"15m", "15m", 21.151, 21.450, 21.300, U, "SSB phone", R1 | R3},
    {"15m", "15m", 21.200, 21.450, 21.300, U, "SSB phone", R2},
    {"12m", "12m", 24.931, 24.990, 24.950, U, "SSB phone", ALL},
    {"10m", "10m", 28.320, 29.000, 28.500, U, "SSB phone", ALL},
    {"10m", "10m FM", 29.510, 29.700, 29.600, N, "FM simplex 29.600, repeaters 29.610 to 29.690", ALL},
    {"6m", "6m", 50.100, 50.500, 50.150, U, "SSB, calling 50.150", R1 | R3},
    {"6m", "6m", 50.100, 50.300, 50.125, U, "SSB, calling 50.125", R2},
    {"6m", "6m FM", 51.410, 51.590, 51.510, N, "FM simplex, calling 51.510", R1 | R3},
    {"6m", "6m FM", 52.000, 54.000, 52.525, N, "FM simplex, calling 52.525", R2},
    {"4m", "4m", 70.100, 70.250, 70.200, U, "SSB, calling 70.200", R1},
    {"4m", "4m FM", 70.250, 70.500, 70.450, N, "FM calling 70.450", R1},
    {"2m", "2m", 144.100, 144.400, 144.300, U, "SSB calling 144.300", ALL},
    {"2m", "2m FM", 145.200, 145.575, 145.500, N, "FM simplex, calling 145.500", R1 | R3},
    {"2m", "2m rpt", 145.600, 145.800, 145.600, N, "FM repeater outputs (RV48 145.600 up)", R1},
    {"2m", "2m FM", 146.400, 146.600, 146.520, N, "FM simplex, calling 146.520", R2},
    {"2m", "2m rpt", 146.600, 147.400, 146.610, N, "FM repeaters (outputs and inputs, offsets vary by area)", R2},
    {"70cm", "70cm", 432.000, 432.400, 432.200, U, "SSB calling 432.200", R1 | R3},
    {"70cm", "70cm", 432.000, 432.400, 432.100, U, "SSB calling 432.100", R2},
    {"70cm", "70cm FM", 433.400, 433.575, 433.500, N, "FM simplex, calling 433.500", R1 | R3},
    {"70cm", "70cm FM", 446.000, 446.100, 446.000, N, "FM simplex, calling 446.000", R2},
};
}  // namespace

std::vector<HamVoiceSeg> hamVoiceSegments(int region) {
    const unsigned bit = region == 2 ? R2 : region == 3 ? R3 : R1;
    std::vector<HamVoiceSeg> v;
    for (const auto& r : kRows)
        if (r.r & bit) v.push_back({r.band, r.btn, r.lo * 1e6, r.hi * 1e6, r.tune * 1e6, r.m, r.m == N ? FM : SSB, r.label, r.r});
    return v;
}

const HamVoiceSeg* hamVoiceSegmentAt(const std::vector<HamVoiceSeg>& segs, double hz) {
    const HamVoiceSeg* best = nullptr;
    for (const auto& s : segs)
        if (hz >= s.loHz && hz <= s.hiHz && (!best || s.hiHz - s.loHz < best->hiHz - best->loHz)) best = &s;
    return best;
}

VoiceMode hamVoiceSideband(double hz) {
    if (hz >= 5.3e6 && hz <= 5.5e6) return VoiceMode::Usb;   // 60 m
    return hz < 10e6 ? VoiceMode::Lsb : VoiceMode::Usb;
}

int hamIaruRegionForPosition(double la, double lo) {
    if (!std::isfinite(la) || !std::isfinite(lo) || std::fabs(la) > 90 || std::fabs(lo) > 360) return 0;
    while (lo > 180) lo -= 360;
    while (lo < -180) lo += 360;
    if (lo >= -170 && lo < -30) return 2;                                   // the Americas, Greenland
    if (lo >= -30 && lo < 44) return 1;                                      // Europe, Africa, west Arabia (to the Caucasus)
    if (lo >= 44 && lo < 63 && la < 26.2) return 1;   // the Arabian peninsula south of Iran (Iraq, the Gulf states)
    if (lo >= 44 && lo < 49 && la < 37) return 1;                            // Iraq, Kuwait, north Saudi Arabia
    if (lo >= 44 && la >= 50) return 1;                                      // Siberia, Russia (the former Soviet Union)
    if (lo >= 44 && lo < 88 && la >= 40 && la < 50) return 1;                // Kazakhstan, Uzbekistan, Turkmenistan, Kyrgyzstan
    if (lo >= 88 && lo < 120 && la >= 41.5 && la < 52) return 1;             // Mongolia
    if (lo >= 40 && lo < 51 && la >= 38 && la < 50) return 1;                // the Caucasus, the Caspian coast, Volga
    return 3;                                                                 // Iran, south and east Asia, Oceania, the Pacific
}

int hamIaruRegionForGrid(const std::string& grid) {
    double la, lo;
    if (!fromMaidenhead(grid, la, lo)) return 0;
    return hamIaruRegionForPosition(la, lo);
}

}  // namespace dect2::ham
