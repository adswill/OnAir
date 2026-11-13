// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RBDS tables and call letters: see fm_rbds.h.
#include "dect2/fm/fm_rbds.h"
#include <cmath>

namespace dect2 {

namespace {

constexpr const char* kRds[32] = {"None", "News", "Current affairs", "Information", "Sport", "Education", "Drama", "Culture", "Science", "Varied",
                                  "Pop music", "Rock music", "Easy listening", "Light classical", "Serious classical", "Other music", "Weather",
                                  "Finance", "Children's", "Social affairs", "Religion", "Phone-in", "Travel", "Leisure", "Jazz music",
                                  "Country music", "National music", "Oldies music", "Folk music", "Documentary", "Alarm test", "Alarm"};
// NRSC-4-B table 2 (programme type codes of the RBDS)
constexpr const char* kRbds[32] = {"None", "News", "Information", "Sports", "Talk", "Rock", "Classic rock", "Adult hits", "Soft rock", "Top 40",
                                   "Country", "Oldies", "Soft", "Nostalgia", "Jazz", "Classical", "Rhythm and blues", "Soft R&B", "Foreign language",
                                   "Religious music", "Religious talk", "Personality", "Public", "College", "Spanish talk", "Spanish music", "Hip hop",
                                   "Unassigned", "Unassigned", "Weather", "Emergency test", "Emergency"};

constexpr int kKBase = 0x1000;            // KAAA
constexpr int kWBase = 0x54A8;            // WAAA: right after KZZZ = 0x1000 + 26^3 - 1 = 0x54A7
constexpr int kLetters = 26 * 26 * 26;    // 17576 call signs per first letter; WZZZ = 0x994F

} // namespace

const char* rdsPtyName(int pty, bool rbds) { return (rbds ? kRbds : kRds)[pty & 31]; }

std::string rbdsCallSign(int pi) {
    if (pi < kKBase || pi >= kWBase + kLetters) return "";
    const bool w = pi >= kWBase;
    int v = pi - (w ? kWBase : kKBase);
    std::string s(4, 'A');
    s[0] = w ? 'W' : 'K';
    s[3] = (char)('A' + v % 26); v /= 26;
    s[2] = (char)('A' + v % 26); v /= 26;
    s[1] = (char)('A' + v % 26);
    return s;
}

int rbdsPiFromCallSign(const std::string& call) {
    if (call.size() != 4) return 0;
    int v = 0;
    for (size_t i = 1; i < 4; i++) {
        const char c = (char)std::toupper((unsigned char)call[i]);
        if (c < 'A' || c > 'Z') return 0;
        v = v * 26 + (c - 'A');
    }
    const char f = (char)std::toupper((unsigned char)call[0]);
    if (f == 'K') return kKBase + v;
    if (f == 'W') return kWBase + v;
    return 0;
}

bool rdsUseRbds(int region, double tunedMhz) {
    if (region == kRdsUs) return true;
    if (region == kRdsEurope) return false;
    const long tenths = std::lround(tunedMhz * 10.0);
    return tunedMhz >= 87.85 && tunedMhz <= 107.95 && std::fabs(tunedMhz * 10.0 - (double)tenths) < 0.05 && (tenths & 1) == 1;
}

} // namespace dect2
