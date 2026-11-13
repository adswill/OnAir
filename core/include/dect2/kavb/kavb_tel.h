// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAVB receiver telemetry. RxTelemetry::kavb, engine standard 125, active standard 124 (family 123), telemetry slot 133.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

// One container packet that arrived whole (CRC-32 checked) from the FEC groups.
struct KavbPacketInfo {
    int type = 0;                 // kavb::PktType: 0 video, 1 audio, 2 text, 3 picture, 4 data
    int stream = 0;
    uint32_t seq = 0;
    uint32_t bytes = 0;           // payload bytes
    double atSec = 0;             // stream time when it arrived
};

struct KavbTelemetry {
    uint64_t seq = 0;
    // 0 no input, 1 searching (no frame header passed its CRC in the last 2 s), 2 locked (frames are being found)
    int state = 0;
    double inputRate = 0, timeSec = 0;
    bool dataValid = false;       // blocks have passed their CRC (the engine's "data valid" flag)
    int listenProfile = 0;        // the symbol rate the receiver is set to (0 A 700, 1 B 1050, 2 C and 3 D 1400 ksym/s)
    double symRate = 0;           // that symbol rate
    // the last frame heard
    int profile = -1;             // 0..3 = A..D from the frame header, -1 none yet
    bool robust = false;
    int fecProfile = 0;           // FEC group profile of the last frame header
    float snrDb = 0;              // = esn0Db (the name the engine copies)
    float esn0Db = 0;             // pilot-based Es/N0 of the last frame
    float cfoHz = 0;              // total carrier offset: the coarse correction plus the frame estimate
    float coarseHz = 0;           // the part of it that the coarse carrier search removed before the receiver
    double lastFrameAgeSec = 0;
    // counts since the start (or the last clear)
    uint64_t framesGood = 0;      // all 4 blocks passed their CRC
    uint64_t framesBad = 0;       // the header decoded but at least one block failed
    uint64_t headerRejects = 0;   // preambles found whose header failed its CRC
    uint64_t blocksOk = 0, blocksBad = 0;
    uint64_t groupsOk = 0, groupsRecovered = 0, groupsLost = 0;   // FEC groups: complete, restored by the erasure code, not restorable
    uint64_t cellsMissing = 0;
    uint64_t packets = 0, packetsDropped = 0;
    uint64_t packetsByType[5] = {};
    uint64_t payloadBytes = 0;    // payload bytes of the packets (fill and the station ID text excluded)
    uint64_t savedBytes = 0;      // bytes written to the output file
    double throughputBps = 0;     // payload bits per second of the packets, averaged over the last 10 s
    std::string callsign;         // from the station ID text packets ("KAVB de <call>") of the OnAir transmitter, empty if none heard
    std::string savePath;         // the output file, empty if none
    std::string saveError;        // why it could not be written
    int constellationBits = 0;
    std::vector<std::pair<float, float>> constellation;   // up to 400 equalised data symbols of the last frame (I, Q)
    std::vector<KavbPacketInfo> recent;                   // newest first, up to 32
};

inline std::string kavbSummary(const KavbTelemetry& t) {
    char b[200];
    if (t.state == 0) return "KAVB: waiting for samples";
    if (t.state == 1) {
        snprintf(b, sizeof b, "KAVB: searching at %.0f ksym/s", t.symRate / 1000.0);
        return b;
    }
    snprintf(b, sizeof b, "KAVB: locked, profile %c, Es/N0 %.0f dB", t.profile >= 0 ? 'A' + t.profile : '?', (double)t.esn0Db);
    return b;
}

} // namespace dect2
