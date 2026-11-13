// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mesh node watch (Meshtastic app notifications, MeshSense): a chosen node not heard for N hours, its battery below X %, a new node near you.
// Turns the node table into lora.node_silent / lora.node_battery / lora.node_new events. docs/MESH-WATCH-3.9.md.
#pragma once
#include "dect2/automation/event.h"
#include "dect2/mesh/mesh_tel.h"
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dect2 {

struct MeshWatchConfig {
    std::set<std::string> watched;     // node ids ("!1a2b3c4d")
    double silentHours = 6;            // 0: off
    double batteryPct = 20;            // 0: off
    double newWithinKm = 0;            // 0: off; needs the receiver position
    bool hasHome = false;
    double homeLat = 0, homeLon = 0;
};

class MeshWatch {
public:
    // the node table now (wall-clock seconds); returns the events to send, each once per state change
    std::vector<automation::Event> update(const std::vector<MeshNode>& nodes, const MeshWatchConfig& c, double wallNow);
    // when each watched node was last heard (wall clock), kept in the settings so a node silent across a restart is still noticed
    std::string serialize() const;
    void parse(const std::string& text);
private:
    struct N { double lastWall = 0; double lastHeardSig = -2; bool silentSent = false, batterySent = false; };
    std::map<std::string, N> n_;
    std::set<std::string> seen_;   // every node seen this session (for "new")
    bool first_ = true;
};

}  // namespace dect2
