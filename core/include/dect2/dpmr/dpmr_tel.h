// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// dPMR receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One transmission: a voice call, a status message, a short message or packet data
struct DpmrCall {
    int carrierId = 0; double carrierHz = 0;   // whole-span decoding: the carrier it came from (id 0: single channel), its frequency relative to the span centre
    int64_t wallTime = 0;        // Unix time of its start
    double startSec = 0, durSec = 0;
    bool active = false;
    int kind = 0;                // 0 voice, 1 status, 2 short message, 3 packet data
    int64_t src = -1, dst = -1;  // own id, called id (24 bits); -1 not known yet
    int callType = 1;            // dpmr::callTypeName
    int commMode = 0;            // dpmr::commModeName
    int cc = -1;                 // colour code
    int channel = 0;             // PMR446 channel 1 to 16 (0: not a PMR446 channel)
    bool emergency = false;
    bool lateEntry = false;      // found from the CCH superframe (the header was missed)
    bool endSeen = false;        // the end frame (FS3) ended it (else the signal was lost)
    uint32_t voiceFrames = 0;
    bool voice = false;          // AMBE+2 decoded to sound
    std::string text;            // status: "status N"; short message / packet: the text
    float berPct = 0;
};

struct DpmrTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 searching, 1 frames seen recently, 2 a call on air
    double cfoHz = 0;            // carrier offset measured on the frame syncs
    float snrDb = 0;             // symbol SNR at the decision points
    float levelDb = -200;        // input level, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;       // header / CCH / end / data blocks with a good CRC
    uint64_t blocksBad = 0;
    // details
    double timeSec = 0, inputRate = 0;
    float deviationHz = 0;       // the +3 symbol's deviation as measured
    bool inverted = false;
    float berPct = 0;            // Hamming corrections per coded bit
    int cc = -1;                 // the last colour code
    uint64_t fs1 = 0, fs2 = 0, fs3 = 0, fs4 = 0;   // frame syncs found
    uint64_t frames = 0, hdrOk = 0, hdrBad = 0, cchOk = 0, cchBad = 0, endOk = 0, endBad = 0, dataOk = 0, dataBad = 0;
    uint64_t voiceFrames = 0, voiceDecoded = 0, lateEntries = 0;
    uint32_t slowData = 0;       // the last CCH slow data (18 bits)
    bool scanning = false;       // scanning the 16 PMR446 channels
    int channel = 0;             // the channel listened to now (PMR446 1 to 16, 0 other)
    bool ambe = false;           // mbelib found
    std::string ambePath;
    DpmrCall now;
    std::vector<DpmrCall> log;   // newest first, at most kDpmrLogMax
    std::vector<std::string> events;   // newest first: decoded headers and end frames as text
};

constexpr size_t kDpmrLogMax = 300;

inline std::string dpmrId(int64_t v) {
    if (v < 0) return "?";
    if (v == 0xFFFFFF) return "ALL";
    char b[16];
    snprintf(b, sizeof b, "%08lld", (long long)v);
    return b;
}

inline std::string dpmrSummary(const DpmrTelemetry& t) {
    char b[200];
    const DpmrCall& n = t.now;
    if (n.active) {
        snprintf(b, sizeof b, "%s -> %s, CC %d, %.1f s", dpmrId(n.src).c_str(), dpmrId(n.dst).c_str(), n.cc, n.durSec);
        return b;
    }
    if (t.frames == 0) {
        if (t.scanning && t.channel) { snprintf(b, sizeof b, "scanning PMR446, channel %d", t.channel); return b; }
        return "searching for dPMR";
    }
    snprintf(b, sizeof b, "dPMR, %zu calls heard", t.log.size());
    return b;
}

} // namespace dect2
