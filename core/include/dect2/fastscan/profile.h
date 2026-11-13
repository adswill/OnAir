// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The fast scan's profile preset (band list, step, dwell, threshold above the floor, minimum duration ...) as the Scanner view edits and saves it,
// the range text it is typed in ("118-137, 156.8, 462.5625-467.7125" in MHz) and the built-in presets. Pure code.
#pragma once
#include "dect2/fastscan/scanner.h"
#include <string>
#include <vector>

namespace dect2 {
namespace fastscan {

struct ScanProfile {
    std::string name = "Custom";
    std::string bands = "118-137";         // MHz ranges, comma separated
    std::string exclusions;                // MHz ranges never reported (known interferers, FM broadcast)
    std::string priorityBands;             // MHz ranges visited more often
    double stepKhz = 12.5;                 // bin width
    double dwellMs = 4;                    // capture per hop after the settle time
    double thresholdDb = 10;               // a hit starts this far above the local floor
    double minDurSec = 0.1;                // shorter bursts are ignored
    double hangSec = 2;                    // a hit lasts this long after the signal is gone
    double channelKhz = 0;                 // > 0: hits snap to this channel raster (airband 25 / 8.33, PMR 12.5 ...)
    bool record = false;                   // record a hit's channel from the wide capture
    bool recordIq = false;                 // the recording is the channel's IQ, not audio
    int maxRecordings = 20;                // 1..100
    double budgetMb = 2048;                // size budget of the scan-recordings folder
    double recordKhz = 12.5;               // width of a recorded channel
};

// "88-108, 121.5, 1090" -> ranges in Hz. A lone number is a range of +-pointKhz around it. Garbage, reversed ranges and empty parts are skipped.
std::vector<FreqRange> parseRanges(const std::string& mhzText, double pointKhz = 6.25);
std::string formatRanges(const std::vector<FreqRange>& r);

std::string serializeProfile(const ScanProfile& p);                // key=value;key=value;...
bool parseProfile(const std::string& s, ScanProfile& out);         // false: nothing readable (out unchanged)
std::vector<ScanProfile> builtinProfiles();

// One-button "Whole span": the tuning range of a radio and the step / dwell / threshold that suit it (wide spans take wider bins and a short dwell).
FreqRange wholeSpan(const RadioProfile& r);
struct AutoScan { double stepKhz = 12.5, dwellMs = 4, thresholdDb = 10; };
AutoScan autoScan(const RadioProfile& r, double spanHz);
ScanProfile wholeSpanProfile(const RadioProfile& r);        // bands = the radio's whole range, automatic step and dwell

// The scanner configuration of a profile. locked: frequencies the tracker ignores; priority rows are the profile's priorityBands.
FastScanConfig toConfig(const ScanProfile& p);

// A hit as a memory row: name from the frequency, the mode guessed, the group "Fast scan".
struct HitMemory { double mhz = 0; std::string mode, name, group; };
HitMemory hitToMemory(const Hit& h, const std::string& mode);

}  // namespace fastscan
}  // namespace dect2
