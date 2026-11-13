// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Analog FPV video channel plans: the 5.8 GHz bands A, B, E, F (Fatshark / ImmersionRC), R (Raceband) and L (Low race), and the 1.2 GHz plan of
// the common 8-channel video transmitters. Frequencies are the nominal channel centres in MHz.
#pragma once
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace dect2 {

struct FpvBand {
    char letter;          // 'A', 'B', 'E', 'F', 'R', 'L', 'G' (the 1.2 GHz plan)
    const char* name;
    double mhz[8];
};

constexpr FpvBand kFpvBands[] = {
    {'A', "A (Boscam A)",         {5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725}},
    {'B', "B (Boscam B)",         {5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866}},
    {'E', "E (Boscam E, Lumenier)", {5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945}},
    {'F', "F (Fatshark)",         {5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880}},
    {'R', "R (Raceband)",         {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917}},
    {'L', "L (Low race)",         {5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621}},
    {'G', "1.2 GHz",              {1080, 1120, 1160, 1200, 1240, 1280, 1320, 1360}},
};
constexpr int kFpvBandCount = (int)(sizeof(kFpvBands) / sizeof(kFpvBands[0]));

struct FpvChannel {
    int band = 0, ch = 0;   // index into kFpvBands, channel 0 .. 7 (named 1 .. 8)
    double freqHz = 0;
    std::string name;       // "R5"
};

inline std::string fpvChannelName(int band, int ch) { return std::string(1, kFpvBands[band].letter) + std::to_string(ch + 1); }

// every channel of every band, in band order
inline std::vector<FpvChannel> fpvAllChannels() {
    std::vector<FpvChannel> v;
    for (int b = 0; b < kFpvBandCount; b++)
        for (int c = 0; c < 8; c++) v.push_back({b, c, kFpvBands[b].mhz[c] * 1e6, fpvChannelName(b, c)});
    return v;
}

// the channel nearest to a frequency, or -1 when none is within maxHz
inline int fpvNearest(const std::vector<FpvChannel>& ch, double hz, double maxHz) {
    int best = -1;
    double bd = maxHz;
    for (size_t i = 0; i < ch.size(); i++) {
        const double d = std::fabs(ch[i].freqHz - hz);
        if (d <= bd) { bd = d; best = (int)i; }
    }
    return best;
}

// the other names of the same frequency (R3 and B1 are a MHz apart, A1 / R7 / F7 close): "R5 / A4 (5806 / 5805)"
inline std::string fpvNamesNear(const std::vector<FpvChannel>& ch, double hz, double withinHz = 2e6) {
    std::string s;
    for (const auto& c : ch) if (std::fabs(c.freqHz - hz) <= withinHz) { if (!s.empty()) s += " / "; s += c.name; }
    return s;
}

} // namespace dect2
