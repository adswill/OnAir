// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi channel plans (see wifi_chan.h). Frequencies follow IEEE 802.11-2020 Annex E.1 (5 GHz: 5000 + 5 n MHz) and 802.11ax-2021 Annex E.4 (6 GHz:
// 5950 + 5 n MHz, channel 2 at 5935 MHz).
#include "dect2/wifi/wifi_chan.h"
#include <algorithm>
#include <cmath>

namespace dect2::wifi {

// Names of the bands, shown in the app and used in saved text.
const char* bandName(int b) { return b == Band24 ? "2.4 GHz" : b == Band5 ? "5 GHz" : b == Band6 ? "6 GHz" : "?"; }

// Names of the regulatory domains (the lists differ, the frequencies do not).
const char* domainName(int d) {
    switch (d) {
    case DomEu: return "Europe (ETSI)";
    case DomUs: return "USA (FCC)";
    case DomJp: return "Japan";
    case DomCn: return "China";
    default: return "All";
    }
}

// Which Wi-Fi band a frequency lies in (5150 - 5895 MHz are the 5 GHz channels 30 - 179, 5925 - 7125 MHz the 6 GHz band).
int bandOfMhz(double m) {
    if (m >= 2400 && m <= 2500) return Band24;
    if (m >= 5150 && m <= 5895) return Band5;
    if (m >= 5925 && m <= 7125) return Band6;
    return -1;
}

// Centre frequency of a channel of a band, 0 for a number that does not exist in that band.
double bandChannelMhz(int band, int ch) {
    if (band == Band24) { if (ch >= 1 && ch <= 13) return 2407 + 5.0 * ch; return ch == 14 ? 2484 : 0; }
    if (band == Band5) return validChannel(Band5, ch) ? 5000 + 5.0 * ch : 0;
    if (band == Band6) return ch == 2 ? 5935 : validChannel(Band6, ch) ? 5950 + 5.0 * ch : 0;
    return 0;
}

// The channel of a band nearest to a frequency, within a tolerance (0: none).
int bandChannelOf(int band, double mhz, double tol) {
    int best = 0;
    double bd = tol + 1e-9;
    const int hi = band == Band24 ? 14 : band == Band5 ? 177 : 233;
    for (int c = 1; c <= hi; c++) {
        const double f = bandChannelMhz(band, c);
        if (f > 0 && std::fabs(f - mhz) < bd) { bd = std::fabs(f - mhz); best = c; }
    }
    return best;
}

// Whether a number is a 20 MHz channel of the band's plan.
bool validChannel(int band, int ch) {
    if (band == Band24) return ch >= 1 && ch <= 14;
    if (band == Band5) return (ch >= 36 && ch <= 64 && ch % 4 == 0) || (ch >= 100 && ch <= 144 && ch % 4 == 0) || (ch >= 149 && ch <= 177 && ch % 4 == 1);
    if (band == Band6) return ch == 2 || (ch >= 1 && ch <= 233 && ch % 4 == 1);
    return false;
}

// The channels a domain lists (EU: 5 GHz 36 - 64 and 100 - 140, 6 GHz 1 - 93 = 5945 - 6425 MHz; US: 5 GHz 36 - 64, 100 - 144, 149 - 177, 6 GHz 1 - 233;
// Japan: 36 - 64, 100 - 140; China: 36 - 64 and 149 - 165).
std::vector<int> channelList(int band, int dom) {
    std::vector<int> v;
    const int hi = band == Band24 ? 14 : band == Band5 ? 177 : 233;
    for (int c = 1; c <= hi; c++) {
        if (!validChannel(band, c)) continue;
        bool ok = true;
        if (band == Band24) ok = dom == DomAll ? true : dom == DomUs ? c <= 11 : c <= 13;
        else if (band == Band5) {
            switch (dom) {
            case DomEu: case DomJp: ok = c <= 140; break;
            case DomCn: ok = c <= 64 || (c >= 149 && c <= 165); break;
            case DomUs: ok = true; break;
            default: ok = true;
            }
        } else if (band == Band6) {
            if (dom == DomEu) ok = c <= 93;
            else if (dom == DomJp) ok = c <= 93;
            else if (dom == DomCn) ok = c <= 93;
            if (c == 2) ok = dom == DomAll || dom == DomUs;
        }
        if (ok) v.push_back(c);
    }
    return v;
}

// 6 GHz preferred scanning channels: 5, 21, 37 ... 229.
std::vector<int> pscChannels() {
    std::vector<int> v;
    for (int c = 5; c <= 229; c += 16) v.push_back(c);
    return v;
}
bool isPsc(int c) { return c >= 5 && c <= 229 && (c - 5) % 16 == 0; }

// DFS (radar detection) channels: 52 - 64 (UNII-2A) and 100 - 144 (UNII-2C).
bool isDfs(int c, int) { return (c >= 52 && c <= 64) || (c >= 100 && c <= 144); }

// UNII sub-band names: 5 GHz UNII-1 36 - 48, UNII-2A 52 - 64, UNII-2C 100 - 144, UNII-3 149 - 165, UNII-4 169 - 177; 6 GHz UNII-5 1 - 93,
// UNII-6 97 - 113, UNII-7 117 - 185, UNII-8 189 - 233.
const char* unii(int band, int c) {
    if (band == Band5) {
        if (c >= 36 && c <= 48) return "UNII-1";
        if (c >= 52 && c <= 64) return "UNII-2A";
        if (c >= 100 && c <= 144) return "UNII-2C";
        if (c >= 149 && c <= 165) return "UNII-3";
        if (c >= 169 && c <= 177) return "UNII-4";
    } else if (band == Band6) {
        if (c >= 1 && c <= 93) return "UNII-5";
        if (c >= 97 && c <= 113) return "UNII-6";
        if (c >= 117 && c <= 185) return "UNII-7";
        if (c >= 189 && c <= 233) return "UNII-8";
    }
    return "";
}

// Width of the global operating classes of 802.11-2020 Annex E.1 that Reduced Neighbor Reports use (81 - 84: 2.4 GHz, 115 - 130: 5 GHz, 131 - 137: 6 GHz).
double operatingClassWidth(int oc) {
    if (oc == 81 || oc == 82 || oc == 115 || oc == 118 || oc == 121 || oc == 124 || oc == 125 || oc == 131 || oc == 136) return 20;
    if (oc == 83 || oc == 84 || oc == 116 || oc == 117 || oc == 119 || oc == 120 || oc == 122 || oc == 123 || oc == 126 || oc == 127 || oc == 132) return 40;
    if (oc == 128 || oc == 133) return 80;
    if (oc == 129 || oc == 134) return 160;
    if (oc == 130 || oc == 135) return 80;   // 80 + 80
    if (oc == 137) return 320;
    return 0;
}
int operatingClassBand(int oc) {
    if (oc >= 81 && oc <= 84) return Band24;
    if (oc >= 115 && oc <= 130) return Band5;
    if (oc >= 131 && oc <= 137) return Band6;
    return -1;
}

// Centre of the BSS's channel from CCFS0 (a channel number whose frequency is the centre of the 40 / 80 / 160 MHz segment); 20 MHz: the primary.
double bssCentreMhz(int band, int ccfs0, int primary) {
    const double f = ccfs0 > 0 ? bandChannelMhz(band, ccfs0) : 0;
    return f > 0 ? f : bandChannelMhz(band, primary);
}

// The wider channel containing a primary (see wifi_chan.h).
int blockCentreChannel(int band, int p, int w) {
    if (w <= 20) return p;
    if (band == Band24) return w == 40 ? p + 2 : 0;   // the secondary above (the one below is p - 2: the caller decides)
    const int span = w / 20 * 4;                        // channel numbers a block covers
    if (band == Band6) {
        if (p < 1 || p > 233) return 0;
        const int start = 1 + (p - 1) / span * span;
        const int c = start + span / 2 - 2;
        return c <= 233 ? c : 0;
    }
    if (!validChannel(Band5, p)) return 0;
    static const int s40[] = {36, 44, 52, 60, 100, 108, 116, 124, 132, 140, 149, 157, 165, 173};
    static const int s80[] = {36, 52, 100, 116, 132, 149, 165};
    static const int s160[] = {36, 100, 149};
    const int* t = w == 40 ? s40 : w == 80 ? s80 : w == 160 ? s160 : nullptr;
    if (!t) return 0;
    const int n = w == 40 ? 14 : w == 80 ? 7 : 3;
    for (int i = 0; i < n; i++) if (p >= t[i] && p < t[i] + span) return t[i] + span / 2 - 2;
    return 0;
}

// Channels of a band whose whole 20 MHz lies inside the capture (the converter's filter rolls off before the edge).
std::vector<int> channelsInSpan(int band, double centreMhz, double rateHz, int dom, double edgeFrac) {
    std::vector<int> v;
    const double half = rateHz / 1e6 * edgeFrac;
    for (int c : channelList(band, dom)) {
        const double f = bandChannelMhz(band, c);
        if (std::fabs(f - centreMhz) + 10.0 <= half + 1e-6) v.push_back(c);
    }
    return v;
}

} // namespace dect2::wifi
