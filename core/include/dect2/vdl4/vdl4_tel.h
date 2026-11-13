// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDL Mode 4 receiver telemetry, shared by the receiver and the UI.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct Vdl4Message {
    uint64_t serial = 0;
    double timeSec = 0;
    int64_t wallTime = 0;
    double freqHz = 0;
    float levelDb = -120;
    float snrDb = 0;
    float cfoHz = 0;
    bool uplink = false;          // ground station transmitted (directed burst from G/S)
    bool downlink = false;        // aircraft / mobile transmitted
    uint32_t src = 0;             // 27-bit source address (lower 24 bits often ICAO)
    int srcType = 0;              // address type field (see vdl4_proto.h)
    bool autonomous = false;      // a/d flag: random slot vs directed
    int burstId = 0;
    int msgId = 0;                // 5-bit VSS message ID
    std::string msgName;          // "SYNC", "DLS", "UCTRL", ...
    std::string summary;
    std::string detail;
    bool syncBurst = false;
    bool hasPos = false;
    double lat = 0, lon = 0;
    bool hasAlt = false;
    int altFt = 0;
    std::string posKind;
    // DLS / CMU-style clear user data (receive only, no decryption)
    bool hasUser = false;
    std::string userHex;
    std::string userText;
};

struct Vdl4TrackPoint { double lat = 0, lon = 0; int altFt = 0; double time = 0; };

struct Vdl4Aircraft {
    uint32_t addr = 0;
    uint32_t messages = 0;
    double freqHz = 0;
    double lastHeardSec = 0;
    int64_t lastWall = 0;
    float levelDb = -120;
    bool hasPos = false;
    double lat = 0, lon = 0;
    bool hasAlt = false;
    int altFt = 0;
    std::string posKind;
    double posTime = 0;
    std::vector<Vdl4TrackPoint> track;
};

struct Vdl4GroundStation {
    uint32_t addr = 0;
    uint32_t messages = 0;
    double freqHz = 0;
    double lastHeardSec = 0;
    float levelDb = -120;
    bool hasPos = false;
    double lat = 0, lon = 0;
};

struct Vdl4ChannelInfo {
    double freqHz = 0;
    bool inBand = false;
    bool active = false;
    float levelDb = -120;
    float noiseDb = -120;
    float snrDb = 0;
    uint64_t bursts = 0;
    uint64_t good = 0;
    uint64_t bad = 0;
    uint64_t frames = 0;
    double lastHeardSec = 0;
    float lastCfoHz = 0;
};

struct Vdl4Telemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 listening, 1 bursts, 2 decoding
    double cfoHz = 0;
    float snrDb = 0;
    bool dataValid = false;
    uint64_t blocksOk = 0;
    uint64_t blocksBad = 0;
    double centerHz = 0;
    double nowSec = 0;
    double inputRate = 0;
    bool rateOk = true;   // false: the sample rate is below what the mode needs (status line says so)
    uint64_t positionsTotal = 0;
    std::vector<Vdl4Message> messages;
    std::vector<Vdl4Aircraft> aircraft;
    std::vector<Vdl4GroundStation> stations;
    std::vector<Vdl4ChannelInfo> channels;
};

constexpr size_t kVdl4MsgMax = 400;

inline std::string vdl4Summary(const Vdl4Telemetry& t) {
    int inBand = 0, act = 0;
    for (const auto& c : t.channels) { inBand += c.inBand; act += c.active; }
    char b[200];
    if (!t.rateOk) {
        snprintf(b, sizeof b, "VDL4: sample rate %.0f kHz is too low, use at least 400 kHz (2 Msps or more with a wide capture)", t.inputRate / 1e3);
        return b;
    }
    if (!t.channels.empty() && !inBand) return "VDL4: no channel inside the band, tune nearer 136.85 MHz";
    if (t.state == 0) {
        snprintf(b, sizeof b, "VDL4: listening to %d channel%s", inBand, inBand == 1 ? "" : "s");
        return b;
    }
    snprintf(b, sizeof b, "VDL4: %s  %d of %d channels active  %llu frames  %llu bad  %zu aircraft  %zu ground stations",
             t.state == 2 ? "decoding" : "bursts", act, inBand, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
             t.aircraft.size(), t.stations.size());
    return b;
}

} // namespace dect2
