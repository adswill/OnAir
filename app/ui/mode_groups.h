// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The headings of the mode lists (kGroupNames / kGroupCols in mode_table.cpp, which hold the texts and colours) as named numbers, and the
// move from the 17 groups of 4.5 to the 35 of 4.6: a saved list of group names (hidden groups, unfolded groups) is stored by NAME, so an old
// name is turned into the new names that took over its modes. UI only; nothing stores a group number. Header only, no ImGui.
#pragma once
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace modegroups {

// the order of kGroupNames (mode_table.cpp)
enum Id : int {
    TV, BroadcastRadio,
    AircraftDatalinks, NavaidsRadar, DronesLightAviation,
    Maritime,
    Navigation, MobileDataSats, CubeSats, RescueTracking, WeatherSatsLow, WeatherSatsHigh, WeatherSondes, Science,
    TwoWayRadio, CellularRail, Pagers,
    SmartDevices, MeshLora, RadioControl, WirelessNetworks, VehiclesRoads,
    AmateurWeak, AmateurText, AmateurImages, AmateurVoicePacket,
    HfData, HfTelegraph, HfMonitoring,
    Receivers, Meters, Interference,
    Plugins, KrisCustom, CustomModes,
    kCount
};

// the groups of 4.5 and the groups of 4.6 that took over their modes
struct OldGroup { const char* name; std::vector<const char*> now; };
inline const std::vector<OldGroup>& oldGroups() {
    static const std::vector<OldGroup> v = {
        {"AVIATION", {"AIRCRAFT DATALINKS", "NAVAIDS & RADAR", "DRONES & LIGHT AVIATION", "RESCUE & TRACKING"}},
        {"SATELLITE & NAVIGATION", {"NAVIGATION", "MOBILE & DATA SATELLITES", "CUBESATS & SMALL SATS", "RESCUE & TRACKING", "WEATHER SATS - LOW RATE"}},
        {"IoT & SENSORS", {"SMART DEVICES & RFID", "MESH & LORA", "RADIO CONTROL & DRONES", "WIRELESS NETWORKS", "VEHICLES & ROADS"}},
        {"WEATHER & SCIENCE", {"WEATHER SATS - LOW RATE", "WEATHER SATS - HIGH RATE", "WEATHER & SONDES", "SCIENCE & SENSING"}},
        {"AMATEUR - DIGITAL", {"AMATEUR - WEAK SIGNAL", "AMATEUR - TEXT MODES", "AMATEUR - IMAGES & DATA", "SCIENCE & SENSING"}},
        {"HF UTILITY", {"HF DATA", "HF TELEGRAPH & FSK", "HF MONITORING"}},
        {"GENERAL & TOOLS", {"RECEIVERS & ANALYSERS", "METERS", "INTERFERENCE HUNTING"}},
    };
    return v;
}

// A saved list of group names ("A|B|C") from an older version, in the names of today: an old name becomes all the names that took over
// its modes, any other name stays. A name that is already new is kept, so the call is safe on a list that was moved before.
inline std::set<std::string> migrate(const std::set<std::string>& in) {
    std::set<std::string> out;
    for (const std::string& n : in) {
        bool old = false;
        for (const OldGroup& o : oldGroups()) if (n == o.name) { old = true; for (const char* g : o.now) out.insert(g); }
        if (!old) out.insert(n);
    }
    return out;
}
inline std::string migrateList(const std::string& list) {
    std::set<std::string> in, out;
    std::stringstream ss(list);
    std::string s;
    while (std::getline(ss, s, '|')) if (!s.empty()) in.insert(s);
    std::string r;
    for (const std::string& g : migrate(in)) { if (!r.empty()) r += "|"; r += g; }
    return r;
}

}  // namespace modegroups
