// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Z-Wave monitor telemetry, shared by the receiver and the UI. The first members are copied into RxTelemetry (history plot, top bar).
#pragma once
#include "dect2/map/mesh_topology.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kZwNetMax = 64;
constexpr size_t kZwNodeMax = 1000;
constexpr size_t kZwLogMax = 600;

struct ZwNetwork {
    uint32_t homeId = 0;
    std::string home;               // "C0FFEE01"
    int nodes = 0;
    int controller = 0;             // node id that sent the network's controller commands (assign IDs, find nodes), else 0
    uint64_t frames = 0, encrypted = 0;
    float rssi = -200;
    double firstSec = 0, lastSec = 0;
    int64_t lastWall = 0;
};

struct ZwNode {
    uint32_t homeId = 0;
    std::string home;
    int id = 0;
    std::string role;               // "controller", "node"
    uint64_t sent = 0, received = 0, encrypted = 0;
    int rate = 0;                   // of its last frame
    float rssi = -200;
    std::string lastCc;             // the command class of its last application frame
    bool beaming = false, lowPower = false;
    double firstSec = 0, lastSec = 0;
    int64_t lastWall = 0;
};

struct ZwFrame {
    int64_t wallTime = 0;
    double sec = 0;
    double mhz = 0;
    int rate = 0;
    std::string home, src, dst;
    std::string type;               // singlecast, multicast, ack, routed
    std::string text;               // describe()
    int seq = 0;
    bool ackReq = false;
    int encryption = 0;             // 0 none, 1 S0, 2 S2
    float rssi = -200, snrDb = 0;
    int len = 0;
    std::string hex;
};

struct ZwChannel {
    double mhz = 0;
    int rate = 0;
    bool covered = false;           // inside the band the radio takes in
    uint64_t frames = 0, bad = 0;
};

struct ZwaveTelemetry {
    uint64_t seq = 0;
    int state = 0;                  // 0 nothing heard, 1 bursts, 2 frames in the last seconds
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;
    double timeSec = 0, inputRate = 0;
    std::string region;             // "EU", "US", ...
    std::string regionName;
    double loMhz = 0, hiMhz = 0;    // the band taken in
    int channelsCovered = 0;
    uint64_t bursts = 0;
    uint64_t perRate[3] = {};
    uint64_t encS0 = 0, encS2 = 0, acks = 0;
    std::vector<ZwChannel> chans;
    std::vector<std::pair<std::string, uint64_t>> perCc;   // command class counts, busiest first
    std::vector<ZwNetwork> networks;   // most recently heard first
    std::vector<ZwNode> nodes;
    std::vector<ZwFrame> log;          // newest first
    TopoSnapshot topo;                 // the mesh of each home ID: repeaters, routes, single points of failure (refreshed about once a second)
};

inline std::string zwaveSummary(const ZwaveTelemetry& t) {
    char b[200];
    if (t.blocksOk == 0) {
        if (t.channelsCovered > 0) { snprintf(b, sizeof b, "listening on %d Z-Wave channel%s (%s)", t.channelsCovered, t.channelsCovered == 1 ? "" : "s", t.region.c_str()); return b; }
        return "no Z-Wave channel in the band taken in";
    }
    snprintf(b, sizeof b, "%zu networks, %zu nodes, %llu frames", t.networks.size(), t.nodes.size(), (unsigned long long)t.blocksOk);
    return b;
}

} // namespace dect2
