// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pure helpers of the receiver status bar (ui2_shell.cpp, statusLine): the frequency digit split and grouping, the CPU level colour and
// which segments of the flat bar stay on a narrow window. No ImGui here, so tests/app/test_status_strip.cpp covers them.
#pragma once
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace statusstrip {
struct FreqParts { std::string lead, rest; };
// "851.013" -> {"851", ".013"}: the first three digits (the dot does not count) are the accent part, the rest is bright
inline FreqParts splitFreq(double mhz) {
    char b[48];
    snprintf(b, sizeof b, "%.3f", mhz < 0 ? 0 : mhz);
    const std::string s = b;
    size_t i = 0;
    int digits = 0;
    while (i < s.size() && digits < 3) { if (s[i] != '.') digits++; i++; }
    return {s.substr(0, i), s.substr(i)};
}
// 0 = normal (amber), 1 = busy (orange), 2 = overloaded (red); pct is the process CPU, 100 = one core
constexpr float kCpuBusy = 150.f, kCpuHot = 250.f;
inline int cpuLevel(float pct) { return pct > kCpuHot ? 2 : pct > kCpuBusy ? 1 : 0; }
// the bar fill 0..1 for a machine with `cores` cores (at least 1)
inline float cpuFill(float pct, int cores) { return std::clamp(pct / (100.f * (float)std::max(1, cores)), 0.f, 1.f); }

// "522.000 000", "1 090.000 000", "93.900 000": the frequency to the hertz, the digits in groups of three on both sides of the dot (the
// flat status bar shows it in the monospaced font, so the groups line up from one frame to the next)
inline std::string groupedMhz(double mhz) {
    char b[48];
    snprintf(b, sizeof b, "%.6f", mhz < 0 ? 0 : mhz);
    const std::string s = b;
    const size_t dot = s.find('.');
    const std::string ip = s.substr(0, dot), fp = dot == std::string::npos ? std::string() : s.substr(dot + 1);
    std::string out;
    for (size_t i = 0; i < ip.size(); i++) { if (i && (ip.size() - i) % 3 == 0) out += ' '; out += ip[i]; }
    if (!fp.empty()) {
        out += '.';
        for (size_t i = 0; i < fp.size(); i++) { if (i && i % 3 == 0) out += ' '; out += fp[i]; }
    }
    return out;
}

// One segment of the flat status bar: its width and how much it matters (0 = kept longest). `on` is the answer of fit().
struct Seg { float w = 0; int prio = 0; bool on = true; };
// The total width of the segments that are on, with `gap` (the divider and its spacing) between neighbours
inline float usedWidth(const std::vector<Seg>& v, float gap) {
    float t = 0; int n = 0;
    for (const Seg& s : v) if (s.on) { t += s.w; n++; }
    return t + (n > 1 ? gap * (float)(n - 1) : 0.f);
}
// Turn segments off until the rest fits in `avail`: the least important first, and of equal ones the rightmost first, so the bar empties
// from the right. Returns the width used. A segment of priority 0 is never dropped.
inline float fit(std::vector<Seg>& v, float avail, float gap) {
    for (Seg& s : v) s.on = true;
    float used = usedWidth(v, gap);
    while (used > avail) {
        int drop = -1;
        for (int i = 0; i < (int)v.size(); i++)
            if (v[i].on && v[i].prio > 0 && (drop < 0 || v[i].prio >= v[drop].prio)) drop = i;
        if (drop < 0) break;
        v[drop].on = false;
        used = usedWidth(v, gap);
    }
    return used;
}
}  // namespace statusstrip
