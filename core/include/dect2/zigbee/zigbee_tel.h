// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Zigbee / IEEE 802.15.4 monitor telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include "dect2/map/mesh_topology.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kZbPanMax = 100;
constexpr size_t kZbDevMax = 600;
constexpr size_t kZbLogMax = 600;
constexpr size_t kZbPpsHist = 120;     // seconds of packets-per-second history

// One network (a PAN ID on a channel)
struct ZbPan {
    uint16_t pan = 0;
    int channel = 0;                   // 11 - 26, 0 unknown (a file)
    std::string extPan;                // from a Zigbee beacon, "" when none heard
    std::string coordinator;           // the address of the PAN coordinator (beacon source, or 0x0000)
    std::string networkName;           // Thread mode: the network name of a Thread beacon (not saved in the results file)
    int threadVersion = -1;            // Thread mode: the version of that beacon
    bool zigbee = false;               // a Zigbee beacon or NWK header was seen
    bool joinOpen = false;             // the last beacon allowed association
    int stackProfile = -1;
    int devices = 0;                   // devices heard in this PAN
    float rssi = -200;                 // dBFS of the last frame (the radio's level, not calibrated dBm)
    uint64_t packets = 0;
    double firstSec = 0, lastSec = 0;  // signal time
    int64_t lastWall = 0;
};

// One device (short or extended address within a PAN)
struct ZbDevice {
    std::string shortAddr;             // "0x1A2B", "" when not heard
    std::string extAddr;               // "00:12:4B:00:01:02:03:04", "" when not heard
    std::string vendor;                // from the OUI of the extended address
    uint16_t pan = 0;
    int channel = 0;
    std::string role;                  // "coordinator", "router", "device" (from what it sends)
    float rssi = -200;
    uint64_t packets = 0;
    double firstSec = 0, lastSec = 0;
    int64_t lastWall = 0;
    std::string last;                  // the last frame's one-line summary
    int shortNum = -1;                 // the short address as a number (-1 none)
    uint64_t extNum = 0;               // the extended address as a number (0 none)
};

struct ZbFrame {
    int64_t wallTime = 0;
    double sec = 0;
    int channel = 0;
    std::string type;                  // Beacon, Data, Ack, MAC cmd
    std::string src, dst;
    uint16_t pan = 0;
    int len = 0;                       // PSDU bytes, FCS included
    bool fcsOk = true;
    bool encrypted = false;
    float rssi = -200;
    std::string info;
    int seq = -1;                      // the MAC sequence number (DSN / BSN), -1 when the frame has none
    bool ackReq = false;               // the sender asks for an acknowledgement (a retry reuses the sequence number)
};

struct ZigbeeTelemetry {
    uint64_t seq = 0;
    int state = 0;                     // 0 nothing heard, 1 frames heard in the last seconds
    double cfoHz = 0;                  // carrier offset of the last good frame
    float snrDb = 0;                   // chip SNR of the last good frames
    float levelDb = -200;              // input level, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;             // frames with a good FCS
    uint64_t blocksBad = 0;            // start-of-frame found, FCS failed
    // details
    double timeSec = 0, inputRate = 0;
    int channel = 0;                   // the channel the radio is tuned to (0: not known, a file)
    int phy = 0;                       // 0 2.4 GHz O-QPSK, 1 868 MHz BPSK, 2 915 MHz BPSK
    uint64_t preambles = 0;            // candidates that went on to the frame decoder
    uint64_t perType[8] = {};          // good frames by MAC frame type
    uint64_t perChannel[16] = {};      // good frames on channels 11 - 26
    uint64_t encryptedFrames = 0;      // frames whose payload is encrypted (not decoded)
    float pps = 0;                     // frames per second, the last second
    std::vector<float> ppsHist;        // frames per second, oldest first, one value a second
    std::vector<ZbPan> pans;           // most recently heard first
    std::vector<ZbDevice> devices;     // most recently heard first
    std::vector<ZbFrame> log;          // newest first, at most kZbLogMax
    TopoSnapshot topo;                 // the mesh of each PAN: links, routers, single points of failure (refreshed about once a second)
};

inline std::string zigbeeSummary(const ZigbeeTelemetry& t) {
    char b[160];
    if (t.blocksOk == 0) {
        if (t.channel) { snprintf(b, sizeof b, "listening on channel %d", t.channel); return b; }
        return "listening for 802.15.4 frames";
    }
    snprintf(b, sizeof b, "%zu networks, %zu devices, %llu frames", t.pans.size(), t.devices.size(), (unsigned long long)t.blocksOk);
    return b;
}

} // namespace dect2
