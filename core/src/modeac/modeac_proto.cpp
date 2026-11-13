// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode A / C reply code (see modeac_proto.h).
#include "dect2/modeac/modeac_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace modeac {

namespace {
// slot -> which bit of which octal digit (0 A, 1 B, 2 C, 3 D; weight 1, 2 or 4), slot 7 is X
struct SlotDef { const char* name; int digit; int weight; };
constexpr SlotDef kSlots[14] = {
    {"F1", -1, 0}, {"C1", 2, 1}, {"A1", 0, 1}, {"C2", 2, 2}, {"A2", 0, 2}, {"C4", 2, 4}, {"A4", 0, 4}, {"X", -1, 0},
    {"B1", 1, 1}, {"D1", 3, 1}, {"B2", 1, 2}, {"D2", 3, 2}, {"B4", 1, 4}, {"D4", 3, 4},
};
}  // namespace

const char* slotName(int slot) { return slot >= 1 && slot <= 13 ? kSlots[slot].name : "?"; }

int squawkOf(uint16_t bits) {
    int d[4] = {0, 0, 0, 0};
    for (int s = 1; s <= 13; s++) if (kSlots[s].digit >= 0 && slotBit(bits, s)) d[kSlots[s].digit] += kSlots[s].weight;
    return d[0] * 1000 + d[1] * 100 + d[2] * 10 + d[3];
}

uint16_t bitsOfSquawk(int squawk) {
    const int d[4] = {squawk / 1000 % 10, squawk / 100 % 10, squawk / 10 % 10, squawk % 10};
    uint16_t b = 0;
    for (int s = 1; s <= 13; s++) if (kSlots[s].digit >= 0 && (d[kSlots[s].digit] & kSlots[s].weight)) b = setSlot(b, s, true);
    return b;
}

namespace {
// The Gray part of the altitude, most significant bit first: D1 D2 D4 A1 A2 A4 B1 B2 B4 (units of 500 ft); the C bits give the 100 ft step
// inside it (five steps; C1 C2 C4 = 001, 011, 010, 110, 100, as in the Mode S decoder's table; 0 ft is 010), run backwards in every second 500 ft block so that one bit changes per step.
constexpr int kGraySlots[9] = {9, 11, 13, 2, 4, 6, 8, 10, 12};   // D1 D2 D4 A1 A2 A4 B1 B2 B4
constexpr int kStepC[6][3] = {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}};   // C1 C2 C4 for step 1 to 5
}  // namespace

Altitude altitudeOf(uint16_t bits) {
    Altitude a;
    if (slotBit(bits, 7)) return a;                       // X is always 0
    const int c1 = slotBit(bits, 1), c2 = slotBit(bits, 3), c4 = slotBit(bits, 5);
    int step = 0;
    for (int s = 1; s <= 5; s++) if (kStepC[s][0] == c1 && kStepC[s][1] == c2 && kStepC[s][2] == c4) step = s;
    if (!step) return a;                                  // 000 and 111 are not in the code
    unsigned gray = 0;
    for (int i = 0; i < 9; i++) gray = (gray << 1) | (unsigned)slotBit(bits, kGraySlots[i]);
    unsigned n = 0;                                       // Gray -> binary
    for (unsigned g = gray; g; g >>= 1) n ^= g;
    if (n & 1) step = 6 - step;
    const int ft = (int)n * 500 + step * 100 - 1300;
    if (ft < -1200 || ft > 126700) return a;
    a.valid = true; a.feet = ft;
    return a;
}

uint16_t bitsOfAltitude(int feet) {
    int i = (int)std::lround((feet + 1200) / 100.0);
    i = std::max(0, std::min(i, 1279));
    const unsigned n = (unsigned)(i / 5);
    int step = i % 5 + 1;
    if (n & 1) step = 6 - step;
    const unsigned gray = n ^ (n >> 1);
    uint16_t b = 0;
    for (int k = 0; k < 9; k++) if ((gray >> (8 - k)) & 1) b = setSlot(b, kGraySlots[k], true);
    b = setSlot(b, 1, kStepC[step][0]); b = setSlot(b, 3, kStepC[step][1]); b = setSlot(b, 5, kStepC[step][2]);
    return b;
}

std::string squawkText(int squawk) {
    char b[8];
    snprintf(b, sizeof b, "%04d", std::max(0, std::min(squawk, 7777)));
    return b;
}

bool isEmergencySquawk(int s) { return s == 7500 || s == 7600 || s == 7700; }

} // namespace modeac
} // namespace dect2
