// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Marine VHF channel plan, 156.000 to 162.025 MHz: channel numbers 1 to 88 with the frequency a ship transmits on and the one a coast station
// transmits on, simplex / duplex, the A / B designators (22A, 87B ...), what each channel is for, and the regional plans (international, Europe,
// Baltic Sea, USA, Canada). The table is data/bandplans/marine_vhf_channels.csv, compiled in (CMake) and parsed here; marineVhfParseCsv() is
// public so a test can check the file against the compiled table.
//
// Sources: ITU Radio Regulations Appendix 18 (the number-to-frequency rule: channels 1 to 28 at 156.050 + 0.05 (n - 1) MHz, channels 60 to 88 at
// 156.025 + 0.05 (n - 60) MHz, a duplex channel's coast frequency 4.6 MHz above the ship's), 47 CFR 80.371, Canadian practice, CEPT and the
// Gulf of Finland reporting system. Uses are the typical ones; national rules differ and decide.
//
// Designators: a plain number (16) is the channel as the region defines it. "A" is the ship transmit frequency used as a simplex channel (US 22A =
// 157.100), "B" the coast transmit frequency used as a simplex channel (87B = 161.975 = AIS 1). A region lists only the designators it uses.
#pragma once
#include <string>
#include <vector>

namespace dect2 {
namespace marinevhf {

enum Region { RegionInternational = 0, RegionEurope, RegionBaltic, RegionUsa, RegionCanada, kRegionCount };

const char* regionCode(Region r);              // "INT", "EUR", "BAL", "USA", "CAN"
const char* regionName(Region r);              // "International (ITU)", ...
bool parseRegion(const std::string& code, Region& r);   // by code, case-insensitive

enum class Use { Voice, Dsc, Ais, LongRangeAis };

enum ChannelFlag : unsigned {
    kDistress = 1u << 0,        // distress and safety
    kCalling = 1u << 1,         // calling channel
    kSar = 1u << 2,             // search and rescue
    kBridge = 1u << 3,          // bridge to bridge (navigation safety)
    kPort = 1u << 4,            // port operations, ship movement, VTS
    kIntership = 1u << 5,       // intership
    kPublic = 1u << 6,          // public correspondence
    kSafetyBroadcast = 1u << 7, // marine safety information broadcasts
    kGovernment = 1u << 8,      // Coast Guard / government
    kLowPower = 1u << 9,        // limited to 1 W
};

struct Channel {
    int number = 0;             // 1 .. 88
    char suffix = 0;            // 0, 'A' or 'B'
    double shipMhz = 0;         // where a ship transmits
    double coastMhz = 0;        // where a coast station transmits; equal to shipMhz on a simplex channel
    Use use = Use::Voice;
    unsigned flags = 0;
    double powerW = 25;
    std::string name;           // short, for the picker: "Distress, safety, calling"
    std::string purpose;        // a sentence

    bool duplex() const { return coastMhz != shipMhz; }
    bool voice() const { return use == Use::Voice; }
    bool has(unsigned f) const { return (flags & f) != 0; }
    std::string label() const;  // "16", "22A", "87B"
    std::string title() const;  // "16 Distress, safety, calling"
    // The frequencies to listen on: both halves of a duplex pair, the one frequency of a simplex channel
    std::vector<double> listenMhz() const;
};

// All channels of a region, ordered by number then suffix ("" before A before B)
const std::vector<Channel>& channels(Region r);

// Lookup by number and suffix (0 = none); nullptr when the region does not use it
const Channel* findChannel(Region r, int number, char suffix = 0);
// "16", "ch 16", "Ch.16", "22a", "VHF 87B" (case and a leading "ch" ignored); with no suffix given and only lettered variants present
// ("22" in the USA), the first variant is returned
const Channel* findLabel(Region r, const std::string& text);
// The channel that transmits on this frequency (ship or coast side) within tolMhz; coastSide is set when it is the coast frequency of a duplex pair
const Channel* findFrequency(Region r, double mhz, double tolMhz = 0.0005, bool* coastSide = nullptr);

// Every distinct frequency of the region's voice channels, ascending (what the whole-span monitor listens on)
std::vector<double> voiceFrequencies(Region r);
// All distinct frequencies including DSC and AIS ones
std::vector<double> allFrequencies(Region r);

// The next / previous voice channel in number order, wrapping around (the up / down buttons); cur may be nullptr (first / last)
const Channel* stepChannel(Region r, const Channel* cur, int dir);

// A channel number from the text of a DSC call ("VHF ch 16", "VHF ch 1016 (ship simplex)", "16", "ch 06"); 0 when there is none.
// A designation of 100 or more keeps its last two digits.
int channelFromDscText(const std::string& text);

// The ITU rule, used by the tests: ship transmit frequency of channel n (1 .. 28, 60 .. 88) in MHz; 0 for a number outside the plan
double ituShipMhz(int number);

// The data file's rows, one per region and designator. regions is the region code; parse errors are listed in errors (a row with the wrong number
// of fields, an unknown region or use). Returns the number of rows kept.
struct CsvRow { Region region; Channel channel; };
size_t marineVhfParseCsv(const std::string& text, std::vector<CsvRow>& out, std::vector<std::string>* errors = nullptr);
const std::string& marineVhfCsvText();   // the compiled-in data/bandplans/marine_vhf_channels.csv

} // namespace marinevhf
} // namespace dect2
