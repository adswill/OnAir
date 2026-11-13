// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Digital video generator, tab "Digital Television Mode Selector": every standard DVB-T, DVB-T2, DVB-T2 narrow band (1,7 MHz), DVB-S,
// DVB-S2 and DVB-S2X mode with its occupied bandwidth, net bitrate and minimum C/N on the AWGN channel, and which of them carry a given
// bitrate in a given bandwidth at a given C/N. The modes and formulas are those of DVB_transmission_modes.xlsx (ETSI EN 300 744, EN 302 755 with
// TS 102 831, EN 300 421, EN 302 307-1 and -2); the standard tables are transcribed in dvb_modes.cpp and the modes generated from them.
// No interface here, so tests/tv/test_dvb_modes.cpp checks all of it.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::vgen {

enum class DvbSystem { T, T2, T2Narrow, S, S2, S2X, Count };   // T2: 5-10 MHz channels; T2Narrow: the 1,7 MHz channel
constexpr int kNumDvbSystems = (int)DvbSystem::Count;
constexpr int kMaxModeParams = 8;   // DVB-T2: bandwidth, FFT, carrier mode, guard interval, pilot pattern, modulation, code rate, FEC frame

const char* dvbSystemName(DvbSystem s);   // "DVB-T", "DVB-T2 1.7 MHz" ...
bool isSatellite(DvbSystem s);            // DVB-S / S2 / S2X: the symbol rate is free, bandwidth and bitrate scale with it

// One parameter of a system's modes (a column of the workbook) and the values it takes, in the order the window lists them
struct ModeParam {
    const char* name;                  // English, translated where it is drawn
    std::vector<std::string> values;   // "8 MHz", "2K", "1/4", "QPSK" ...
};
const std::vector<ModeParam>& modeParams(DvbSystem s);

struct DvbMode {
    DvbSystem system = DvbSystem::T;
    std::array<int8_t, kMaxModeParams> value{};   // per parameter of modeParams(system): the index into its values
    double bwMHz = 0;      // occupied bandwidth; satellite: at 1 Msym/s, so 1 + roll-off
    double rateMbps = 0;   // net bitrate (useful TS / data-field rate); satellite: at 1 Msym/s, so the spectral efficiency in bit/symbol
    double cnDb = 0;       // minimum C/N on the AWGN channel (satellite: noise bandwidth = symbol rate)
    bool hasCn = true;     // false where the standard gives no threshold (DVB-S2 short FECFRAME): never counted as available
};
// All modes of a system, built once (DVB-T 480, DVB-T2 28 560, DVB-T2 1,7 MHz 5 712, DVB-S 5, DVB-S2 312, DVB-S2X 714 with every roll-off)
const std::vector<DvbMode>& dvbModes(DvbSystem s);
std::string modeName(const DvbMode& m);   // "T-8MHz-8K-GI1/4-64-QAM-2/3", "S2-8PSK-3/4-N-POn-RO0.20" ...

// What the link offers: the bitrate to carry, the bandwidth on the spectrum and the C/N seen on the spectrum analyser
struct LinkInput {
    double kbps = 0;
    double bwMHz = 0;
    double cnDb = 0;
};

// How a mode does on that link
struct ModeFit {
    bool ok = false;            // carries the bitrate within the bandwidth at that C/N
    double bwMHz = 0;           // the bandwidth it occupies: terrestrial the channel's, satellite at the symbol rate the bitrate needs
    double capacityMbps = 0;    // the most it carries in the available bandwidth
    double symbolRate = 0;      // satellite: Msym/s needed for the bitrate (0 for terrestrial)
    double marginDb = 0;        // link C/N minus the mode's minimum
};
ModeFit fitMode(const DvbMode& m, const LinkInput& in);

// A filter per parameter: -1 leaves it open, otherwise the index of the wanted value
using ModeFilter = std::array<int, kMaxModeParams>;
ModeFilter openFilter();
bool matches(const DvbMode& m, const ModeFilter& f);

struct ModeChoice {
    const DvbMode* mode = nullptr;
    ModeFit fit;
};
// The modes of a system that pass the filter and work on the link, the best first: the smallest bandwidth, then the most C/N margin,
// then the most capacity
std::vector<ModeChoice> availableModes(DvbSystem s, const ModeFilter& f, const LinkInput& in);
bool betterChoice(const ModeChoice& a, const ModeChoice& b);

} // namespace dect2::vgen
