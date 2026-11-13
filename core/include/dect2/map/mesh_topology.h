// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Passive mesh topology, shared by every mesh-like mode: Meshtastic / MeshCore (LoRa), Zigbee and Thread (IEEE 802.15.4), Z-Wave.
//
// MeshTopology keeps the nodes a receiver has heard of and the links between them, built only from what the frames say:
//   Heard     the receiver itself heard the node (an edge to the observer node; used for SNR and to rank relay candidates, not for the graph)
//   Link      one hop that a header proves: a MAC unicast from one node to the next, a Meshtastic packet that took exactly one hop, an ack
//   Route     a hop that an end-to-end route names (Z-Wave route header, Zigbee source route, MeshCore path) without it being seen itself
//   Inferred  a statistical guess (a deeper copy of a flood that followed a single shallower relay); drawn dashed, off by default
// Ambiguity is kept, not hidden: a Meshtastic relay byte (the last byte of a node number) that fits several known nodes spreads its weight over
// them, weighted by who the receiver can hear directly, and the edge is flagged ambiguous.
// analyse() ranks the relays (share of the relayed traffic), gives the flood amplification factor (copies heard per packet) and finds the
// single nodes whose loss would cut other nodes off from the root (the coordinator, the controller, or the best connected node).
// Pure code: no clock, no threads, no state outside the object; times are seconds supplied by the caller.
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dect2 {

enum class TopoEvidence { Heard = 0, Link = 1, Route = 2, Inferred = 3 };

struct TopoNode {
    std::string id;            // unique key, the feed prefixes the group ("1A2B/0x0001", "C0FFEE01/5", "!1a2b3c4d")
    std::string label;         // what to show ("0x0001", "node 5", "Kris base")
    std::string group;         // the network: PAN, home ID, "meshtastic" ... analysis is per group
    std::string role;          // "coordinator", "router", "device", "ROUTER", ... as the protocol states it, "" unknown
    bool observer = false;     // the receiver itself: kept out of the analysis
    bool placeholder = false;  // only a relay byte or hash was seen, the node itself is not known yet
    double first = 0, last = 0;
    uint32_t originated = 0;   // packets it started, as far as the headers say
    double relayed = 0;        // copies it sent on for others (fractional when the relay byte was ambiguous)
    uint32_t heard = 0;        // frames the receiver heard from it directly
    double snrSum = 0; uint32_t snrN = 0;   // of those frames
    bool hasPos = false; double lat = 0, lon = 0;
};

struct TopoEdge {
    std::string a, b;          // the two nodes, a < b (links are treated as undirected)
    double link = 0, route = 0, inferred = 0, heard = 0;   // weight per kind (fractional for ambiguous relays)
    double snrSum = 0; uint32_t snrN = 0; float lastSnr = 0;
    bool ambiguous = false;    // part of the weight came from a relay byte that fitted several nodes
    double first = 0, last = 0;
    double weight() const { return link + route; }
    double meanSnr() const { return snrN ? snrSum / snrN : 0; }
};

struct TopoRelayRank { std::string id; double relayed = 0, share = 0; };             // share 0..1 of all relayed copies in the group
struct TopoCut { std::string id; int orphans = 0; std::vector<std::string> cutOff; };  // nodes unreachable from the root without `id`

struct TopoReport {
    std::string group, root;
    int nodes = 0, links = 0;      // links counted with Link + Route evidence (and Inferred when asked)
    int components = 0;            // connected pieces of that graph
    double floodFactor = 0;        // copies heard per distinct packet, over packets heard at least once (0: no flood data)
    uint32_t floodPackets = 0, floodCopies = 0;
    std::vector<TopoRelayRank> relays;   // busiest first
    std::vector<TopoCut> cuts;           // worst first: only nodes whose loss orphans somebody
    std::string advice;                  // one line for the user
};

struct TopoSnapshot {
    std::vector<TopoNode> nodes;
    std::vector<TopoEdge> edges;
    std::vector<TopoReport> reports;     // one per group
    bool empty() const { return nodes.empty(); }
};

class MeshTopology {
public:
    explicit MeshTopology(size_t maxNodes = 600, size_t maxEdges = 2000) : maxNodes_(maxNodes), maxEdges_(maxEdges) {}
    void clear();

    // ---- nodes ----
    // Creates the node when new; an empty group keeps the old one.
    TopoNode& node(const std::string& id, const std::string& group, double t);
    void setLabel(const std::string& id, const std::string& label);
    void setRole(const std::string& id, const std::string& role);
    void setPosition(const std::string& id, double lat, double lon);
    void markObserver(const std::string& id);
    const TopoNode* find(const std::string& id) const;
    size_t nodeCount() const { return nodes_.size(); }

    // ---- evidence ----
    // `weight` below 1 marks a guess spread over several candidates (the edge becomes ambiguous).
    void link(const std::string& from, const std::string& to, TopoEvidence ev, double t, bool hasSnr = false, float snr = 0, double weight = 1);
    void originated(const std::string& id, double t) { ++node(id, "", t).originated; }
    void relayedBy(const std::string& id, double t, double weight = 1) { node(id, "", t).relayed += weight; }
    // The receiver heard `id` itself: counts the frame and its SNR and adds the Heard edge to the observer.
    void heardDirect(const std::string& id, const std::string& observer, double t, float snr);

    // One copy of a flooded packet was heard (a Meshtastic id, a MeshCore hash): feeds the amplification factor.
    void floodCopy(const std::string& group, const std::string& packetKey);

    // ---- Meshtastic relay bytes ----
    // The relay byte of a copy tells who sent it. Candidates are the known nodes of the group whose id is "!xxxxxxxx" ending in `byte`,
    // `exclude` (the origin) left out. Weights (sum 1) prefer nodes the receiver hears directly. Empty: nobody known (the caller makes a placeholder).
    std::vector<std::pair<std::string, double>> relayCandidates(const std::string& group, int byte, const std::string& exclude) const;

    // ---- analysis ----
    // root "" : the observer's neighbour that is best connected... in practice the best connected node of the group.
    TopoReport analyse(const std::string& group, const std::string& root = std::string(), bool withInferred = false) const;
    TopoSnapshot snapshot(bool withInferred = false) const;   // copies, with a report for every group

    const std::unordered_map<std::string, TopoNode>& nodes() const { return nodes_; }
    const std::map<std::pair<std::string, std::string>, TopoEdge>& edges() const { return edges_; }

private:
    void trim();
    size_t maxNodes_, maxEdges_;
    std::unordered_map<std::string, TopoNode> nodes_;
    std::map<std::pair<std::string, std::string>, TopoEdge> edges_;
    struct Flood { uint32_t packets = 0, copies = 0; std::unordered_map<std::string, uint16_t> seen; std::deque<std::string> order; };
    std::map<std::string, Flood> flood_;
};

} // namespace dect2
