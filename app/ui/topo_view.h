// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The mesh topology picture shared by the Meshtastic / MeshCore, Zigbee / Thread and Z-Wave screens: a graph of the nodes of one network (rings
// around the root by hop distance), links drawn by evidence and SNR, relays as squares, single points of failure ringed in red, and the
// report beside it (busiest relays, who would be cut off, flood amplification, one line of advice). Data: TopoSnapshot (core mesh_topology.h).
#pragma once
#include "dect2/map/mesh_topology.h"
#include <string>

struct App;
// `id` keeps the chosen network and the Inferred switch per screen. Draws into the space left in the current window.
void topoView(App& a, const char* id, const dect2::TopoSnapshot& snap);
