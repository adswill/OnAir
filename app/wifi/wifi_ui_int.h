// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi screens, shared pieces: the persistent state (band, regulatory domain, hop list and dwell), the radio checks, the channel scan and
// the channel occupancy view. wifi_ui.cpp holds the network table and the mode's hooks.
#pragma once
#include "app.h"
#include "dect2/wifi/wifi_tel.h"
#include <string>
#include <vector>

namespace wfui {

struct State {
    int view = 0;                        // 0 networks, 1 beacons, 2 channels
    bool loaded = false, hop = false;
    int band = 0;                        // 0 2.4 GHz, 1 5 GHz, 2 6 GHz: the band selector (channel list, hop list)
    int domain = 0;                      // dect2::wifi::Domain
    int dwellMs = 200;                   // time on each channel while hopping (a beacon comes every 102.4 ms)
    bool pscOnly = true;                 // 6 GHz: hop over the preferred scanning channels 5, 21, 37 ... only
    bool skipDfs = false;                // 5 GHz: leave the radar-sharing channels out of the hop list
    double lastHop = 0;
    char filter[64] = "";
    float minRssi = -120;
    bool openOnly = false;
    int bandFilter = -1;                 // table: -1 all bands, else only that band
};
extern State S;

void loadState();
bool live(const App& a);                                  // the receiver of this mode is the one running
std::vector<int> hopList();                               // the channels the hop steps through, from band, domain and the options
void tuneChannel(App& a, int band, int ch);               // retune the radio to a channel of a band (the network table stays)
std::string radioNote(const App& a);                      // "" or why the radio cannot listen to the chosen band / sample rate
void tunerControls(App& a, bool& retune);                 // band, domain, channel, hop and dwell
void hopTick(App& a);                                     // call every frame
void channelsView(App& a);                                // channel occupancy chart and the per-band summary
const char* stdWord(const std::string& standard);         // the newest generation of "a/n/ac/ax": "ax" ...

} // namespace wfui
