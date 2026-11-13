// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (802.11ah) channel plans of the regions (halow_chan.h): the band edges and allowed widths per country, the S1G channel numbers
// (centre = start + 0.5 MHz x number) and the operating classes. See the note in halow_chan.h on what is certain and what is the author's assignment.
#include "dect2/halow/halow_chan.h"
#include <algorithm>
#include <cmath>

namespace dect2::halow {

namespace {

// the regions, in the order of the Region enum
const std::vector<RegionInfo>& table() {
    static const std::vector<RegionInfo> t = {
        {RegUS, "US", "United States / Canada", 902.0, {{902, 928}}, {1, 2, 4, 8, 16}},
        {RegEU, "EU", "Europe (ETSI)", 863.0, {{863, 868}}, {1, 2}},
        {RegJP, "JP", "Japan", 916.5, {{916.5, 927.5}}, {1, 2, 4}},
        {RegKR, "KR", "South Korea", 917.5, {{917.5, 923.5}}, {1, 2, 4}},
        {RegSG, "SG", "Singapore", 863.0, {{866, 869}, {920, 925}}, {1, 2, 4}},
        {RegAU, "AU", "Australia", 902.0, {{915, 928}}, {1, 2, 4, 8}},
        {RegNZ, "NZ", "New Zealand", 902.0, {{915, 928}}, {1, 2, 4, 8}},
        {RegCN, "CN", "China", 755.0, {{755, 779}, {779, 787}}, {1, 2, 4}},
        {RegIN, "IN", "India", 863.0, {{865, 867}}, {1, 2}},
        {RegTW, "TW", "Taiwan", 902.0, {{920, 928}}, {1, 2, 4, 8}},
    };
    return t;
}

// operating class numbers per region and width (1, 2, 4, 8, 16 MHz); 0 = the region has no such width. US 1 - 5 and EU 6 - 7 as in Annex E, the rest assigned in order.
int classOf(int region, int widthMhz) {
    static const int cls[kNumRegions][5] = {
        {1, 2, 3, 4, 5}, {6, 7, 0, 0, 0}, {8, 9, 10, 0, 0}, {11, 12, 13, 0, 0}, {14, 15, 16, 0, 0},
        {17, 18, 19, 20, 0}, {17, 18, 19, 20, 0}, {21, 22, 23, 0, 0}, {24, 25, 0, 0, 0}, {26, 27, 28, 29, 0}};
    const int wi = widthMhz == 1 ? 0 : widthMhz == 2 ? 1 : widthMhz == 4 ? 2 : widthMhz == 8 ? 3 : widthMhz == 16 ? 4 : -1;
    return wi < 0 || region < 0 || region >= kNumRegions ? 0 : cls[region][wi];
}

// the channel starting frequency of an operating class number (clauses as above)
double classStart(int opClass) {
    if (opClass >= 1 && opClass <= 5) return 902.0;
    if (opClass == 6 || opClass == 7) return 863.0;
    if (opClass >= 8 && opClass <= 10) return 916.5;
    if (opClass >= 11 && opClass <= 13) return 917.5;
    if (opClass >= 14 && opClass <= 16) return 863.0;
    if (opClass >= 17 && opClass <= 20) return 902.0;
    if (opClass >= 21 && opClass <= 23) return 755.0;
    if (opClass == 24 || opClass == 25) return 863.0;
    if (opClass >= 26 && opClass <= 29) return 902.0;
    return 0.0;
}

} // namespace

const RegionInfo& regionInfo(int region) { return table()[(size_t)(region < 0 ? 0 : region >= kNumRegions ? kNumRegions - 1 : region)]; }

int operatingClass(int region, int widthMhz) { return classOf(region, widthMhz); }

// Tiles of one width in every band (4 / 8 / 16 MHz of the 902 MHz classes on the 904 MHz grid, see below): the first centre is half a width above the band edge, the next one a width further, while the channel stays inside the band.
std::vector<Chan> regionChannels(int region, int widthMhz) {
    const RegionInfo& r = regionInfo(region);
    std::vector<Chan> v;
    if (std::find(r.widths.begin(), r.widths.end(), widthMhz) == r.widths.end()) return v;
    for (const Band& b : r.bands) {
        // 4 / 8 / 16 MHz channels of the 902 MHz operating classes sit on a grid that starts 2 MHz above 902 (US 4 MHz: 8, 16 ... 48 = 906 ... 926 MHz,
        // the top one touches 928 and none touches 902; 8 MHz: 908, 916, 924; 16 MHz: 912); narrower ones and the other regions tile from the band edge
        const double origin = (r.startMhz == 902.0 && widthMhz >= 4) ? 904.0 : b.loMhz;
        const double first = origin + widthMhz / 2.0 + std::ceil((b.loMhz - origin) / widthMhz - 1e-9) * widthMhz;
        for (double c = first; c + widthMhz / 2.0 <= b.hiMhz + 1e-6; c += widthMhz) {
            const int ccf = (int)std::lround((c - r.startMhz) * 2.0);
            if (ccf > 0 && ccf < 256) v.push_back({ccf, r.startMhz + 0.5 * ccf, widthMhz});
        }
    }
    return v;
}

// Every channel of every allowed width of a region.
std::vector<ChanRow> channelTable(int region) {
    const RegionInfo& r = regionInfo(region);
    std::vector<ChanRow> t;
    for (int w : r.widths) for (const Chan& c : regionChannels(region, w)) t.push_back({r.code, classOf(region, w), c.ccf, c.mhz, w});
    return t;
}

// Centre frequency of an S1G channel from its operating class and number; 0 when the class is not known.
double chanMhz(int opClass, int ccf) {
    const double s = classStart(opClass);
    return ccf <= 0 || s <= 0 ? 0.0 : s + 0.5 * ccf;
}

// The number of the channel of that width nearest to a frequency (within 1 MHz), 0 when none.
int nearestChannel(int region, double mhz, int widthMhz, double* chMhz) {
    int best = 0; double bd = 1.0;
    for (const Chan& c : regionChannels(region, widthMhz)) if (std::fabs(c.mhz - mhz) < bd) { bd = std::fabs(c.mhz - mhz); best = c.ccf; if (chMhz) *chMhz = c.mhz; }
    return best;
}

// The first region whose band holds the frequency, -1 when none.
int regionOf(double mhz) {
    for (const RegionInfo& r : table()) for (const Band& b : r.bands) if (mhz >= b.loMhz - 1e-6 && mhz <= b.hiMhz + 1e-6) return (int)r.id;
    return -1;
}

bool inHalowBand(double mhz) { return regionOf(mhz) >= 0; }

std::vector<Chan> euChannels(int widthMhz) { return regionChannels(RegEU, widthMhz); }
std::vector<Chan> usChannels(int widthMhz) { return regionChannels(RegUS, widthMhz); }
int nearestChannel(double mhz, int widthMhz, bool eu, double* chMhz) { return nearestChannel(eu ? (int)RegEU : (int)RegUS, mhz, widthMhz, chMhz); }

} // namespace dect2::halow
