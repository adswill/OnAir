// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The small card shown next to the aircraft picked on the VDL2, HFDL and ACARS maps (#394): registration, aircraft type (from the aircraft
// database the ADS-B screen uses, aircraft.csv), flight, ICAO address, altitude, and the distance and bearing from the user's own location
// (the ADS-B position setting and its nm / km unit). It saves opening the table. See ac_card.cpp.
#pragma once
#include "imgui.h"
#include <cstdint>
#include <string>

namespace adsbmap {

struct AcCard {
    uint32_t icao = 0;             // 0: no address known (the type then cannot be looked up)
    std::string reg, flight;       // as received; the database registration is used when `reg` is empty
    std::string source;            // how the position was learnt and how old it is, one line ("" none)
    bool hasAlt = false;
    int altFt = 0;
    double lat = 0, lon = 0;       // the position drawn
};

// Draws the card (an ImGui window, no title bar) beside `marker` (screen position), kept inside the map rectangle (mapP0, mapSize).
// `id` must differ per screen.
void aircraftCard(const char* id, ImVec2 mapP0, ImVec2 mapSize, ImVec2 marker, const AcCard& c);

}
