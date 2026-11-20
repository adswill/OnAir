// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Streamer mode (Settings > Streamer, File > Streamer mode): what to keep off the screen while streaming or sharing it. One master switch and
// a switch per kind of detail; each helper is true only while the master is on, so the panels ask one question.
//   location  the station position (latitude, longitude, locator, name, height): the maps, the markers and range rings, distance and bearing
//             from home and the region guess treat the position as "not set"
//   network   the addresses typed in Settings (UDP output host, update mirror, map tile server)
//   paths     the data folder
#pragma once
#include "../platform/platform.h"
#include <cstring>

namespace streamer {

inline bool on() { return plat::prefs().getB("streamerMode", false); }
inline bool hideLocation() { return on() && plat::prefs().getB("streamerHideLocation", true); }
inline bool hideNetwork() { return on() && plat::prefs().getB("streamerHideNetwork", true); }
inline bool hidePaths() { return on() && plat::prefs().getB("streamerHidePaths", true); }

// a Settings row whose value is hidden now (the row shows a note instead of the value)
inline bool hiddenKey(const char* k) {
    static const char* const loc[] = {"adsbLat", "adsbLon", "stationLocator", "stationName", "rxAltM"};
    static const char* const net[] = {"udpHost", "updMirror", "mapTileUrl"};
    static const char* const path[] = {"dataFolder"};
    if (hideLocation()) for (const char* x : loc) if (!std::strcmp(k, x)) return true;
    if (hideNetwork()) for (const char* x : net) if (!std::strcmp(k, x)) return true;
    if (hidePaths()) for (const char* x : path) if (!std::strcmp(k, x)) return true;
    return false;
}

}  // namespace streamer
