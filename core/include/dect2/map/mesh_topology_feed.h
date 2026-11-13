// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Feeds a MeshTopology (mesh_topology.h) from what each protocol layer decodes. One data model, four front ends:
//   LoRa     Meshtastic (hop_limit / hop_start / relay byte of every copy) and MeshCore (the path hashes of a flood)
//   802.15.4 Zigbee (MAC addresses, NWK originator, source route) and Thread (MAC addresses, 6LoWPAN mesh header), per PAN
//   Z-Wave   the MAC source / destination, acks and the route header of routed frames, per home ID
// Nothing is decrypted: only headers that are in the clear are used. The observer node "rx" stands for the receiver itself.
#pragma once
#include "dect2/map/mesh_topology.h"
#include "dect2/mesh/mesh_tel.h"
#include "dect2/thread/thread_proto.h"
#include "dect2/zigbee/zigbee_proto.h"
#include "dect2/zwave/zwave_proto.h"
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace dect2 {

constexpr const char* kTopoObserver = "rx";

// What the LoRa feed remembers between packets: who relayed each packet at which depth (for the inferred edges), and which packets were counted.
struct LoraTopoState {
    struct Seen { int depth; std::string relay; };
    std::unordered_map<std::string, std::vector<Seen>> perPacket;
    std::deque<std::string> order;
};

// A node-table entry of the mesh monitor: role, name, position (any may be empty / unknown).
void topoNoteMeshNode(MeshTopology& t, const std::string& id, const std::string& name, const std::string& role, bool hasPos, double lat, double lon, double now, int protocol = 1);
// One decoded LoRa packet (Meshtastic and MeshCore are used, others ignored). `now` is any monotonic time in seconds.
void topoFeedLora(MeshTopology& t, LoraTopoState& st, const MeshPacket& p, double now);
// One Z-Wave frame whose check passed; snrDb is the receiver's estimate.
void topoFeedZwave(MeshTopology& t, const zwave::Frame& f, float snrDb, double now);
// One 802.15.4 frame (Zigbee, or Thread when `th` is given). `pan` is the PAN the frame was seen in.
void topoFeedIeee(MeshTopology& t, const zigbee::Frame& f, const thread::Frame* th, uint16_t pan, float snrDb, double now);

std::string topoGroupName(const std::string& group);   // "PAN 1A2B" for a bare 4 hex digits, else as given

} // namespace dect2
