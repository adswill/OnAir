// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF beacons: the beacon list (editable JSON, shipped default), the NCDXF/IARU International Beacon Project schedule and the planning of
// radio centre frequencies for a narrow capture. Pure functions, no state. Sources of the default list are named per beacon:
//  - NCDXF/IARU IBP: ncdxf.org/beacon (transmission schedule text: 3 minute cycle, 10 s slots, callsign + four 1 s dashes at 100 W, 10 W, 1 W, 0.1 W,
//    "steps to the next higher band and the next beacon in the sequence"); the 18 beacons' order and grid squares are the IBP table
//  - Long Dashes: sigidwiki.com/wiki/Long_Dashes (8 frequencies, 2.9 s dashes, 27 s cycle in descending frequency, central Australia suspected)
//  - Russian single-letter beacons: priyom.org/military-stations/russia/single-letter-beacons (ten clusters, 100 Hz slots, markers D P S C A F K M)
#pragma once
#include <string>
#include <vector>

namespace dect2::hfb {

enum Kind { Carrier = 0, Ncdxf = 1, Letter = 2, Dashes = 3 };

struct Beacon {
    std::string name;
    double khz = 0;
    int kind = Carrier;
    char letter = 0;            // Letter: the Morse marker (D P S C A F K M)
    int ncdxfIdx = -1;          // Ncdxf: 0..17, the beacon's place in the sequence
    int ncdxfBand = -1;         // Ncdxf: 0..4 for 14.100, 18.110, 21.150, 24.930, 28.200 MHz
    int dashIdx = -1;           // Dashes: place in the 27 s cycle (0 = 29996 kHz first dash; the second dash on 29996 is place 1)
    double lat = 0, lon = 0;
    std::string mode;           // "CW", "OOK", ...
    std::string schedule;
    std::string source;
    bool uncertain = false;     // frequency, place or identity not firmly given by the source
    double searchHz = 0;        // frequency tolerance searched around khz (0: the kind's default)
};

constexpr int kNcdxfBeacons = 18, kNcdxfBands = 5, kNcdxfSlotSec = 10, kNcdxfCycleSec = 180;
extern const char* const kNcdxfCall[kNcdxfBeacons];
extern const double kNcdxfBandKhz[kNcdxfBands];
// Which beacon sends on `band` during the slot containing UTC second `utcSec` (slot 0 = 00:00:00 of each 3 minute cycle, as the IBP defines it), and which slot
int ncdxfBeaconAt(int band, double utcSec);
int ncdxfSlotAt(double utcSec);                       // 0..17
// The next start (UTC seconds) of beacon `idx` on `band`, at or after utcSec
double ncdxfNextStart(int idx, int band, double utcSec);

double searchOf(const Beacon& b);                     // the tolerance used: searchHz, else 150 Hz (carrier, NCDXF), 50 Hz (letters), 1000 Hz (dashes)
const char* kindName(int kind);
std::string bandName(double khz);                     // "20 m", ... or "" outside the HF broadcast / amateur bands used here
int bandIndexOf(double khz);                          // 0 = 80 m and below ... see bandName; -1 none

std::vector<Beacon> defaultBeacons();
std::string toJson(const std::vector<Beacon>& b);
// false (nothing changed, err says why) when the text is not a beacon list
bool fromJson(const std::string& text, std::vector<Beacon>& out, std::string* err = nullptr);

struct Centre { double centerHz = 0; std::vector<int> beacons; };
// The radio centres that cover every beacon with a capture of `rateHz` (usable span 80 % of the rate), fewest first; beacons of `only` kind
// (-1: all) are planned. A centre never lies below 100 kHz.
std::vector<Centre> planCentres(const std::vector<Beacon>& b, double rateHz, int onlyKind = -1);

} // namespace dect2::hfb
