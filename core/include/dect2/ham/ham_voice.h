// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The amateur voice band plan for the General receiver: per IARU region the voice segments of 160 m to 10 m (SSB, lower sideband below
// 10 MHz and upper sideband above, 60 m upper by convention) and the 6 m, 4 m (Region 1), 2 m and 70 cm SSB calling areas and FM
// simplex / repeater segments. One click tunes a segment and sets its demodulator and filter width (docs/ham-voice.md).
// Receive aid only: it is not a transmit permission and the national band plan of the operator's country is the authority. Pure data
// and functions, no state.
#pragma once
#include <string>
#include <vector>

namespace dect2::ham {

enum class VoiceMode { Nfm, Usb, Lsb };

struct HamVoiceSeg {
    const char* band;     // "20m"
    const char* button;   // what the button says: "20m", "10m FM", "2m rpt"
    double loHz, hiHz;    // the segment
    double tuneHz;        // where a click tunes (a calling frequency or the middle of the segment)
    VoiceMode mode;
    double bwHz;          // filter width: 2.7 kHz for SSB, 12.5 kHz for FM
    const char* label;    // "SSB phone", "FM simplex, calling 145.500"
    unsigned regions;     // bit 0 region 1, bit 1 region 2, bit 2 region 3
};

// The segments of an IARU region (1, 2 or 3), in band order. Any other region number gives region 1.
std::vector<HamVoiceSeg> hamVoiceSegments(int region);
// The segment of the region that holds hz (the narrowest one when two overlap), or nullptr
const HamVoiceSeg* hamVoiceSegmentAt(const std::vector<HamVoiceSeg>& segs, double hz);
// The sideband the voice convention uses at hz: LSB below 10 MHz, USB from 10 MHz up (60 m is the exception: USB)
VoiceMode hamVoiceSideband(double hz);

// The IARU region (1, 2 or 3) of a position, approximately: the Americas and Greenland are 2; Africa, Europe, the Middle East west of Iran,
// and the countries of the former Soviet Union and Mongolia are 1; the rest of Asia and Oceania are 3. Border areas can be wrong by a
// country: the manual choice is always there.
int hamIaruRegionForPosition(double latDeg, double lonDeg);
// Same from a Maidenhead locator (4 or 6 characters); 0 when it is not a locator
int hamIaruRegionForGrid(const std::string& grid);

}  // namespace dect2::ham
