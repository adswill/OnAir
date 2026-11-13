// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// APRS / packet: who repeated what. From the digipeater path of every frame heard (the H bits mark the hops that happened) a live graph of
// stations, digipeaters and IGates is built, with the stations each digipeater relayed, their spread on the map, and flags for digipeaters that
// loop packets or have gone quiet. Works off air and offline: no IGate connection needed. Pure data in, pure data out (no radio, no clock).
#pragma once
#include "packet_ax25.h"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dect2 {

// One digipeater or IGate heard in a path
struct PacketDigi {
    std::string call;
    uint32_t heard = 0;          // frames we received straight from it (it was the last hop of the path)
    uint32_t hops = 0;           // frames whose path it appears in as a hop that happened (also earlier in the chain)
    uint32_t copies = 0;         // of those received frames, how many were repeats of a packet heard before (the same text through another way)
    uint32_t loops = 0;          // frames with this call twice in the path, or the same packet again and again through it
    uint32_t stations = 0;       // different stations it relayed to us
    double firstSec = 0, lastSec = 0;
    bool igate = false;          // sends packets that came from the internet (TCPIP in the path behind it)
    bool silent = false;         // heard often before, but nothing for much longer than its usual interval (at snapshot time)
    bool looping = false;        // loops >= 3 and more than one frame in ten
    bool hasPos = false;         // its own position, when it beacons one
    double lat = 0, lon = 0;
    double rangeKm = 0;          // farthest relayed station from its own position (from the centre of them when it has none)
    std::vector<std::pair<float, float>> hull;   // convex hull (lat, lon) of the relayed stations' positions, at most 24 corners
};

// A hop that was seen: from -> to (stations and digipeaters by call)
struct PacketPathEdge {
    std::string from, to;
    uint32_t count = 0;
    double lastSec = 0;
};

class PacketPathGraph {
public:
    // One received frame. copy: the packet was heard before (the receiver's duplicate check). hasPos / lat / lon: the position of the SOURCE
    // when this frame (or an earlier one) told it.
    void observe(const ax25::Frame& f, double tSec, bool hasPos, double lat, double lon, bool copy);
    // The graph as lists, digipeaters most heard first. posOf: the own position of a station by call (a digipeater's beacon).
    void snapshot(double nowSec, const std::function<bool(const std::string&, double&, double&)>& posOf, std::vector<PacketDigi>& digis, std::vector<PacketPathEdge>& edges) const;
    void shiftTimes(double dt);
    void clear();
    uint32_t directFrames() const { return direct_; }   // frames heard with no hop at all

    static bool isAlias(const std::string& callNoSsid);  // WIDE1, WIDE2, RELAY, TRACE, TCPIP ...: a path word, not a station

private:
    struct Node {
        uint32_t heard = 0, hops = 0, copies = 0, loops = 0, frames = 0;
        double first = 0, last = 0;
        bool igate = false;
        std::map<std::string, std::pair<double, double>> via;   // relayed stations: call -> last known position (NaN lat = unknown)
    };
    struct Edge { uint32_t count = 0; double last = 0; };
    std::unordered_map<std::string, Node> nodes_;
    std::map<std::pair<std::string, std::string>, Edge> edges_;
    struct Seen { uint32_t n = 0; double first = 0; };
    std::unordered_map<std::string, Seen> again_;           // packet text + last digipeater -> times heard
    uint32_t direct_ = 0;
};

} // namespace dect2
