// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ARIB STD-B24 8-unit text (ISDB-T / ISDB-S service names and guide): see dvb_text.h. Kanji, hiragana and katakana go out through EUC-JP.
#include "dect2/tv/dvb_text.h"
#include <cstring>
#include <string>

namespace dect2::dvbsi {

namespace {
enum Set { Kanji, Alnum, Hira, Kata, Other };
struct Slot { Set s; int bytes; };
Slot designate(int f, int bytes) {
    if (bytes == 2) return {Kanji, 2};
    switch (f) { case 0x4A: case 0x36: return {Alnum, 1}; case 0x30: case 0x37: return {Hira, 1}; case 0x31: case 0x38: case 0x49: return {Kata, 1}; default: return {Other, 1}; }
}
void emit(std::string& out, const Slot& s, int b1, int b2) {
    uint8_t e[2];
    switch (s.s) {
    case Alnum: out += (char)b1; break;
    case Hira: e[0] = 0xA4; e[1] = (uint8_t)(b1 | 0x80); out += convertCharset("EUC-JP", e, 2); break;
    case Kata: e[0] = 0xA5; e[1] = (uint8_t)(b1 | 0x80); out += convertCharset("EUC-JP", e, 2); break;
    case Kanji: e[0] = (uint8_t)(b1 | 0x80); e[1] = (uint8_t)(b2 | 0x80); out += convertCharset("EUC-JP", e, 2); break;
    default: out += ' '; break;
    }
}
}  // namespace

std::string aribText(const uint8_t* p, int n) {
    Slot g[4] = {{Kanji, 2}, {Alnum, 1}, {Hira, 1}, {Kata, 1}};
    int gl = 0, gr = 2, single = -1;
    std::string out;
    int i = 0;
    while (i < n) {
        const int c = p[i++];
        if (c == 0x1B) {
            if (i >= n) break;
            const int d = p[i++];
            if (d == 0x6E) gl = 2; else if (d == 0x6F) gl = 3; else if (d == 0x7E) gr = 1; else if (d == 0x7D) gr = 2; else if (d == 0x7C) gr = 3;
            else if (d >= 0x28 && d <= 0x2B) { if (i < n) { int f = p[i++]; g[d - 0x28] = designate(f, 1); } }
            else if (d == 0x24) {
                if (i >= n) break;
                int e = p[i++];
                if (e >= 0x28 && e <= 0x2B) { if (i < n) { int f = p[i++]; g[e - 0x28] = designate(f, 2); } }
                else g[0] = designate(e, 2);
            }
            continue;
        }
        if (c == 0x0F) { gl = 0; continue; } if (c == 0x0E) { gl = 1; continue; }
        if (c == 0x19) { single = 2; continue; } if (c == 0x1D) { single = 3; continue; }
        if (c == 0x0D) { out += '\n'; continue; }
        if (c == 0x20 || c == 0xA0) { out += ' '; continue; }
        if (c < 0x20 || c == 0x7F) continue;
        if (c >= 0x80 && c <= 0xA0) {
            if (c == 0x9B) { while (i < n && p[i] < 0x40) i++; i++; continue; }             // CSI
            if (c == 0x90 || c == 0x92 || c == 0x97) { if (i < n) i++; }                    // COL, CDC, FLC: one parameter
            continue;
        }
        const bool left = c < 0x80;
        const int cc = c & 0x7F;
        const Slot s = g[single >= 0 ? single : left ? gl : gr];
        single = -1;
        if (s.bytes == 2) { if (i < n) { emit(out, s, cc, p[i] & 0x7F); i++; } }
        else emit(out, s, cc, 0);
    }
    return out;
}

bool looksArib(const uint8_t* p, int n) {
    for (int i = 0; i < n; i++) if (p[i] >= 0x80 || p[i] == 0x1B || p[i] == 0x0E || p[i] == 0x0F) return true;
    return false;
}

}  // namespace dect2::dvbsi
