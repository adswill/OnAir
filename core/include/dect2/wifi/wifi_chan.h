// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi channel plans for 2.4, 5 and 6 GHz (IEEE 802.11-2020 Annex E, 802.11ax-2021 Annex E.4): channel number <-> centre frequency,
// the 20 MHz channels of every band, UNII sub-bands, DFS and preferred scanning channels (PSC), regulatory domains, and the hop lists the
// scanner steps through. Receive only: the domain only selects which channels are listened to, nothing is transmitted.
#pragma once
#include <string>
#include <vector>

namespace dect2::wifi {

enum Band { Band24 = 0, Band5 = 1, Band6 = 2 };
enum Domain { DomAll = 0, DomEu, DomUs, DomJp, DomCn, kDomains };   // what the domain list may hold: see domainName()

const char* bandName(int band);                       // "2.4 GHz", "5 GHz", "6 GHz"
const char* domainName(int dom);                      // "All", "Europe (ETSI)", "USA (FCC)", "Japan", "China"
int bandOfMhz(double mhz);                            // 0 / 1 / 2 for the Wi-Fi bands, -1 outside them
double bandChannelMhz(int band, int ch);              // centre of the 20 MHz channel: 2.4 GHz 2407 + 5 ch (14: 2484), 5 GHz 5000 + 5 ch, 6 GHz 5950 + 5 ch (ch 2: 5935); 0 invalid
int bandChannelOf(int band, double mhz, double tolMhz = 2.5);   // the channel of that band within tolMhz of mhz, 0 none
bool validChannel(int band, int ch);                  // a channel of the plan (5 GHz: 36 - 64, 100 - 144, 149 - 177; 6 GHz: 1, 5, 9 ... 233 and 2)
std::vector<int> channelList(int band, int domain);   // the 20 MHz channels allowed in the domain, ascending
std::vector<int> pscChannels();                       // the 6 GHz preferred scanning channels 5, 21, 37 ... 229 (every 16th, FILS discovery and probe responses)
bool isPsc(int ch6);
bool isDfs(int ch5, int domain = DomAll);             // 5 GHz radar-sharing channels: 52 - 64 and 100 - 144 (EU, US and Japan)
const char* unii(int band, int ch);                   // "UNII-1" ... "UNII-4" (5 GHz), "UNII-5" ... "UNII-8" (6 GHz), "" none
double operatingClassWidth(int opClass);              // MHz of a global operating class used by the Reduced Neighbor Report (131 - 136 are 6 GHz), 0 unknown
int operatingClassBand(int opClass);                  // 1 for the 5 GHz classes (115 - 130), 2 for 131 - 137, 0 for 2.4 GHz (81 - 84), -1 unknown
// The 20 MHz primary channel's centre -> the BSS channel's centre, from the segment-0 centre channel (CCFS0) and the width
double bssCentreMhz(int band, int ccfs0, int primary);
// The centre channel number (CCFS0) of the 40 / 80 / 160 / 320 MHz channel that contains the 20 MHz primary: 5 GHz blocks start at 36, 44, 52, 60, 100 ...
// (80 MHz centres 42, 58, 106, 122, 138, 155, 171; 160 MHz centres 50, 114, 163), 6 GHz blocks start at channel 1 and are aligned to their width
// (802.11-2020 Annex E.1, 802.11ax-2021 Annex E.4). 20 MHz: the primary itself; 0 when the primary has no such block
int blockCentreChannel(int band, int primary, int widthMhz);
// Channels of band that a capture centred at centreMhz with the given sample rate covers wholly (edgeFrac of the rate, like spanSlots)
std::vector<int> channelsInSpan(int band, double centreMhz, double rateHz, int domain = DomAll, double edgeFrac = 0.46);

} // namespace dect2::wifi
