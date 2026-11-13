// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode A / C transponder replies (the old secondary-radar replies on 1090 MHz, before Mode S): the framing, the squawk (Mode A) and the
// Gillham altitude code (Mode C).
//
// A reply is 15 pulse positions 1.45 us apart, each 0.45 us wide: the framing pulse F1 first, then thirteen information positions
// C1 A1 C2 A2 C4 A4 X B1 D1 B2 D2 B4 D4, then the framing pulse F2 (20.3 us after F1). The SPI ("ident") pulse follows F2 by 4.35 us.
// Mode A replies carry the squawk as four octal digits (A B C D, each from its 1 / 2 / 4 pulses); Mode C replies carry the pressure altitude
// in 100 ft steps in the Gillham (Gray) code. The reply itself does not say which of the two it is: the interrogation decides.
// Written from the public description of the code (ICAO Annex 10, Vol. IV); the Gray structure is checked by tests (one bit per 100 ft).
#pragma once
#include <cstdint>
#include <string>

namespace dect2 {
namespace modeac {

constexpr double kSlotUs = 1.45;       // pulse position pitch
constexpr double kPulseUs = 0.45;      // pulse width
constexpr int kSlotF2 = 14;            // F2 is the fifteenth position
constexpr int kSlotSpi = 17;           // SPI: 4.35 us after F2
constexpr double kReplyUs = 24.65 + kPulseUs;   // from F1 to the end of the SPI position

// The thirteen information positions as one word: slot 1 (C1) is bit 12, slot 13 (D4) is bit 0.
inline bool slotBit(uint16_t bits, int slot) { return (bits >> (13 - slot)) & 1; }
inline uint16_t setSlot(uint16_t bits, int slot, bool on) { return on ? (uint16_t)(bits | (1u << (13 - slot))) : (uint16_t)(bits & ~(1u << (13 - slot))); }

// Squawk as a decimal number written like the digits: 7700 for A=7 B=7 C=0 D=0.
int squawkOf(uint16_t bits);
uint16_t bitsOfSquawk(int squawk);     // X = 0; 0 to 7777 with octal digits only (others are masked)

struct Altitude {
    bool valid = false;                // the C bits and the Gray part form a Gillham code, X is 0 and the altitude is in range
    int feet = 0;                      // pressure altitude, 100 ft steps, -1200 to 126700
};
Altitude altitudeOf(uint16_t bits);
uint16_t bitsOfAltitude(int feet);     // the nearest 100 ft step, clamped to the range; X = 0

const char* slotName(int slot);        // "C1" ... "D4" for slot 1 to 13
std::string squawkText(int squawk);    // "7700"
bool isEmergencySquawk(int squawk);    // 7500 hijack, 7600 radio failure, 7700 general emergency

} // namespace modeac
} // namespace dect2
