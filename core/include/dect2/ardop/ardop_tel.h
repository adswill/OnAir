// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ARDOP receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kArdopFramesMax = 300;
constexpr size_t kArdopStationsMax = 100;
constexpr size_t kArdopTextMax = 4000;
constexpr size_t kArdopHist = 120;

struct ArdopFrameLog {
    int64_t utc = 0;             // seconds
    double timeSec = 0;          // signal seconds
    int code = 0;                // the frame type byte
    std::string name;            // "ConReq500M", "4FSK.500.100.E", "DataACK"
    std::string kind;            // "connect request", "ID", "data", ...
    std::string mod;             // data frames: 4FSK / 4PSK / 8PSK / 16QAM
    int baud = 0, bandwidth = 0; // Hz class and symbol rate of a data frame
    int sid = -1;                // session ID (the type XOR the second type byte), 255 = unconnected / broadcast
    std::string from, to, locator;
    std::string text;            // printable payload
    std::string hex;             // payload bytes
    int quality = -1;            // ACK / NAK / PingAck quality (38 .. 100)
    int snrDb = -1000;           // PingAck report
    int leaderMs = -1;           // ConAck
    bool headerOk = false;       // the type was read with a good parity check
    bool bodyRead = false;       // the body of this frame type is read by the receiver (control frames: nothing to read)
    bool bodyOk = false;         // CRC / Reed-Solomon passed
    int rsFixed = 0;
    float toneSnrDb = 0;         // from the tone energies of the frame type symbols
    double offsetHz = 0;         // how far the tones sat from their nominal frequency
};

struct ArdopStation {
    std::string call, locator;
    uint64_t count = 0;
    int64_t firstSeen = 0, lastSeen = 0;
    std::string last;            // the last frame type heard from it
    std::string heardTo;         // the station it called last
};

struct ArdopTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 nothing, 1 a leader or frame is being read, 2 frames decoded lately
    double cfoHz = 0;            // tone offset of the last frame
    float snrDb = 0;             // tone SNR of the last frame
    float levelDb = -200;        // input power, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;       // frames read with a good header
    uint64_t blocksBad = 0;      // leaders whose frame type was unreadable
    // details
    double timeSec = 0, inputRate = 0;
    uint64_t leaders = 0, dataOk = 0, dataBad = 0, rsCorrections = 0, controlFrames = 0, otherModulations = 0;
    std::vector<ArdopFrameLog> frames;   // newest first
    std::vector<ArdopStation> stations;  // most recently heard first
    std::string text;                    // payload of the data frames that passed their CRC, the newest kArdopTextMax characters
    std::vector<float> levelHist;        // tone-band level per second, dB
};

inline std::string ardopSummary(const ArdopTelemetry& t) {
    char b[160];
    if (t.blocksOk) { snprintf(b, sizeof b, "ARDOP: %llu frames, %zu stations", (unsigned long long)t.blocksOk, t.stations.size()); return b; }
    if (t.state >= 1) return "ARDOP: leader heard, no frame yet";
    return "ARDOP: no signal";
}

} // namespace dect2
