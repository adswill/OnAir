// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (802.11ah) channel plans of the regions that allow sub-GHz Wi-Fi: country, band, S1G channel number, centre frequency, width and
// operating class. The S1G channel centre frequency is  start + 0.5 MHz x channel number  (IEEE 802.11-2020 clause 23.3.? / Annex E, certain); the
// channels of a band are the non-overlapping tiles of each allowed width from the lower band edge up (the way the regulatory plans are cut).
// UNVERIFIED: the operating class NUMBERS other than US 1 - 5 and EU 6 - 7 are the author's assignment (Annex E was not at hand), and which widths
// a regulator allows in a band is taken from public regulatory summaries. Used by the app channel list, the channel scan and the beacon parser.
#pragma once
#include <string>
#include <vector>

namespace dect2::halow {

enum Region { RegUS = 0, RegEU, RegJP, RegKR, RegSG, RegAU, RegNZ, RegCN, RegIN, RegTW, kNumRegions };

struct Chan { int ccf; double mhz; int widthMhz; };               // S1G channel number, centre frequency, width
struct ChanRow {
    const char* country;                                          // "US", "EU", ...
    int opClass;                                                  // operating class (Annex E numbering, see the note above)
    int ccf;                                                      // channel number (S1G channel centre frequency index)
    double mhz;                                                   // centre frequency
    int widthMhz;
};
struct Band { double loMhz, hiMhz; };
struct RegionInfo {
    Region id;
    const char* code;                                             // "US", "EU", "JP", ...
    const char* name;                                             // "United States / Canada"
    double startMhz;                                              // the channel starting frequency of the S1G operating classes
    std::vector<Band> bands;                                      // allowed bands, e.g. 902 - 928
    std::vector<int> widths;                                      // allowed channel widths (MHz)
};

const RegionInfo& regionInfo(int region);                         // clamped to the known regions
std::vector<ChanRow> channelTable(int region);                    // every channel of every width, in frequency order per width
std::vector<Chan> regionChannels(int region, int widthMhz);       // the channels of one width
int operatingClass(int region, int widthMhz);                     // 0 when the region has no such width
double chanMhz(int opClass, int ccf);                             // frequency from an S1G Operation element; 0 when the class is not known
int nearestChannel(int region, double mhz, int widthMhz, double* chMhz = nullptr);   // channel number of the nearest channel within 1 MHz, 0 none
int regionOf(double mhz);                                         // the first region whose band holds the frequency (US before AU / NZ / TW), -1 none
bool inHalowBand(double mhz);                                     // in any region's band

// Narrow compatibility entry points used by the first HaLow monitor (EU and US plans only)
std::vector<Chan> euChannels(int widthMhz);
std::vector<Chan> usChannels(int widthMhz);
int nearestChannel(double mhz, int widthMhz, bool eu, double* chMhz = nullptr);

} // namespace dect2::halow
