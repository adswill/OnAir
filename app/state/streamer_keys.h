// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Which saved settings Streamer mode keeps from the screen (see ui/streamer.h). No ImGui, no platform: the settings store (rx_prefs.cpp) asks it for
// every key it is given, so a panel that reads the saved position or callsign directly gets "not set" with nothing to change in the panel.
#pragma once
#include <cstring>

namespace streamer {

enum class KeyClass { None, Location, Callsign };

inline KeyClass keyClass(const char* k) {
    static const char* const loc[] = {"adsbLat", "adsbLon", "uatLat", "uatLon", "rfiLat", "rfiLon", "prTxLat", "prTxLon", "mcLat", "mcLon",
                                      "setupLoc", "stationLocator", "stationName", "rxAltM", "wsprNetLocator", "wsprNetLocatorOwn",
                                      "pskrLocator", "pskrLocatorOwn", "winlinkGrid", "winlinkGridOwn", "zwMyHome"};
    static const char* const call[] = {"packetMyCall", "winlinkCall", "wsprNetCall", "sondeHubCall", "pskrCall", "feedAprsCall",
                                       "echolinkCall", "cubesatSidsCall", "pwsCwCall", "rotSerial"};
    for (const char* x : loc) if (!std::strcmp(k, x)) return KeyClass::Location;
    for (const char* x : call) if (!std::strcmp(k, x)) return KeyClass::Callsign;
    return KeyClass::None;
}

}  // namespace streamer
