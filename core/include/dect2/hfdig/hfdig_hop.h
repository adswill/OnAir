// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Band hopping for one narrow radio (WSJT-X Band Hopping): a band per 2-minute slot, chosen from the bands switched on for each UTC hour,
// optionally following the coordinated hopping table (the same band at every station in the same slot). docs/HFDIG-HOP-3.9.md.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

// Band numbers in a plan: 0 .. 10 are the built-in HF / 6 m bands (hfdig_mbsel.h), kHopUserBase + i is the user's own frequency i (issue #501:
// a DXpedition on 14.091 MHz, a frequency the WSJT-X list does not have). One mask bit per band.
constexpr int kHopUserBase = 11;
constexpr int kHopUserMax = 20;                   // kHopUserBase + kHopUserMax <= 32 bits of the hour mask
struct HopUserFreq {
    double hz = 0;                                // the dial frequency
    int mode = 0;                                 // the mode it is used for: an ftx mode number (0 FT8, 1 FT4, 2 FT2, 3 WSPR, 4 JT65 ... 10 JT4)
    std::string label;                            // optional name ("DXpedition")
    bool enabled = true;                          // off: kept in the list, never hopped to
};

struct HopPlan {
    bool coordinated = true;                 // the 20-minute cycle 160 80 60 40 30 20 17 15 12 10 m, a band per 2-minute slot
    std::array<uint32_t, 24> hours{};        // per UTC hour: bit b = band b (0 = 160 m ... 10 = 6 m, kHopUserBase + i = user frequency i) switched on
    std::vector<HopUserFreq> user;           // the user's own frequencies (at most kHopUserMax)
    int slotSec = 120;
    static HopPlan allHours(uint32_t bands) { HopPlan p; p.hours.fill(bands); return p; }
};

// the band for the slot holding utc; -1 when no band is on in that hour. Coordinated: the table's band for the slot, or when that band is off,
// the next band on in the table's order. Not coordinated: the bands on in that hour in turn, one per slot.
// mode >= 0: only the user frequencies of that mode count (a JT65 plan does not hop to an FT8 frequency); -1: every enabled one.
// A user frequency is not part of the coordinated 20-minute table: with Coordinated on it is used only in an hour where no table band is on;
// with Coordinated off it takes its turn in the rotation like any band.
int hopBand(const HopPlan& p, int64_t utc, int mode = -1);
int64_t hopNextChange(const HopPlan& p, int64_t utc);   // the start of the next slot
// the dial frequency (Hz) of a plan band for a mode: built-in bands from the WSJT-X table (0 when the mode has none there), user bands their own
double hopDialHz(const HopPlan& p, int band, int mode);
std::string hopBandName(const HopPlan& p, int band);   // "20 m", or the user label, or "14.0910 MHz"
// Add / replace a user frequency (same frequency and mode = replace). "" on success, else a sentence (the plan is unchanged): 
// no frequency below 100 kHz or above 1.3 GHz, the list is full.
std::string hopAddUser(HopPlan& p, const HopUserFreq& f);
void hopRemoveUser(HopPlan& p, int index);              // removes entry i and shifts the plan's bits of the later entries down
// "" when hz is in an amateur band (IARU plan, any region) and at the usual place for a mode; else a warning sentence only (receiving and
// hopping are never blocked; the user decides before they transmit).
std::string hopUserWarning(double hz, int mode);
// "c=1;h=<24 hex masks separated by ,>;u=<hz>:<mode>:<enabled>:<label %-escaped>|..."
std::string hopToText(const HopPlan& p);
HopPlan hopFromText(const std::string& s);

}  // namespace dect2
