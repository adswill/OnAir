// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pure helpers of the receiver strip (receivers.cpp): label shortening, chip widths, Ctrl+digit mapping, tab scroll.
#pragma once
#include <algorithm>
#include <cstdio>
#include <string>

namespace rxbar {
// the label "1 · ADS-B · 1090.000 MHz"; the mode name is what gives way first (see ellipsize)
inline std::string label(int rx, const char* mode, double mhz) {
    char b[160];
    snprintf(b, sizeof b, "%d \xC2\xB7 %s \xC2\xB7 %.3f MHz", rx, mode, mhz);
    return b;
}
// cut s to at most maxChars UTF-8 characters, ending in "..." (an ellipsis character) when cut; never splits a multi-byte character
inline std::string ellipsize(const std::string& s, int maxChars) {
    if (maxChars <= 0) return "";
    int n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
    if (n <= maxChars) return s;
    if (maxChars == 1) return "\xE2\x80\xA6";
    int keep = maxChars - 1, seen = 0;
    size_t i = 0;
    for (; i < s.size(); i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) { if (seen == keep) break; seen++; }
    }
    return s.substr(0, i) + "\xE2\x80\xA6";
}
// width of a chip given the text width wanted: clamped to [minW, maxW]
inline float chipWidth(float textW, float extras, float minW, float maxW) { return std::clamp(textW + extras, minW, maxW); }
// Ctrl+1..9 -> index into the receiver list (0-based), -1 when the digit has no receiver
inline int indexForDigit(int digit, int count) { return digit >= 1 && digit <= 9 && digit <= count ? digit - 1 : -1; }
// scroll offset that brings the chip [x0, x1] fully into a viewport of width vw, starting from scroll
inline float scrollToShow(float scroll, float x0, float x1, float vw) {
    if (x0 < scroll) return x0;
    if (x1 > scroll + vw) return x1 - vw;
    return scroll;
}
}  // namespace rxbar
