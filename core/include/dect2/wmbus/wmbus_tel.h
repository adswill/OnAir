// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wireless M-Bus receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include "wmbus_proto.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kWmbusLogMax = 200;
constexpr size_t kWmbusDevMax = 300;
constexpr size_t kWmbusHist = 120;

struct WmbusChannelInfo {
    std::string name;            // "868.300 MHz"
    double freqHz = 0;
    std::string modes;           // "S1" / "T1 C1" / "R2" / "N1"
    float levelDb = -200;        // channel power, dBFS
    uint64_t frames = 0, bad = 0;
};

struct WmbusFrameLog {
    int64_t wallTime = 0;        // UTC seconds
    int phy = 0;                 // wmbus::Phy
    int channel = 0;
    float rssiDb = -200;
    bool crcOk = false;
    bool encrypted = false;
    std::string mfr, id, type, summary, hex;
};

struct WmbusDevice {
    std::string mfr, id, type;
    uint8_t version = 0;
    int phy = 0;
    uint64_t count = 0;
    int64_t firstSeen = 0, lastSeen = 0;
    float rssiDb = -200;
    bool encrypted = false;
    std::string last;            // the most recent readings in one line
    std::vector<wmbus::Record> records;
    std::string status;
};

struct WmbusTelemetry {
    uint64_t seq = 0;
    int state = 0;                // 0 no signal, 1 energy on a channel, 2 frames decoded
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;
    // details
    double timeSec = 0, inputRate = 0, centreHz = 0;
    uint64_t syncs = 0, encryptedFrames = 0, falseSync = 0;
    std::vector<WmbusChannelInfo> channels;
    std::vector<WmbusDevice> devices;      // most recently heard first
    std::vector<WmbusFrameLog> log;        // newest first
    std::vector<float> levelHist;          // strongest channel, per second
    std::vector<int16_t> trace;            // discriminator of the last frame, +-16384 = the nominal deviation
};

inline std::string wmbusSummary(const WmbusTelemetry& t) {
    char b[160];
    if (t.blocksOk) { snprintf(b, sizeof b, "Wireless M-Bus: %zu meters, %llu frames", t.devices.size(), (unsigned long long)t.blocksOk); return b; }
    if (t.state >= 1) return "Wireless M-Bus: signal on a channel, no frame yet";
    return "Wireless M-Bus: no signal";
}

} // namespace dect2
