// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CubeSat telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include "cubesat_proto.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kCubeFramesMax = 300;     // frames kept

struct CubeFrame {
    double timeSec = 0;                 // signal time
    int64_t wallTime = 0;               // the computer's clock (0: a file)
    int framing = 0;                    // cubesat::Framing
    int baud = 0;
    int rsErrors = -1;                  // bytes the Reed-Solomon code put right (-1: no RS code)
    std::string sat;                    // the satellite from the table ("" unknown)
    int norad = 0;
    // AX.25
    std::string from, to, path, type;   // callsigns, digipeaters, frame type ("UI", ...)
    int pid = -1;
    // AX100 / CCSDS
    bool hasCsp = false;
    cubesat::CspHeader csp;
    std::vector<uint8_t> data;          // AX.25: the frame without its FCS; AX100: the CSP packet (header included); CCSDS: the data field
    std::vector<uint8_t> payload;       // AX.25: the information field; AX100: after the CSP header; CCSDS: = data
    bool forwarded = false;             // sent to the KISS feed
    float snrDb = 0;
};

struct CubeTelemetry {
    uint64_t seq = 0;
    int state = 0;                      // 0 no signal, 1 a carrier, 2 frames decoded lately
    double cfoHz = 0;                   // the carrier's offset from the dial (Doppler left over after the pass planner's correction)
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0;              // frames decoded
    uint64_t blocksBad = 0;             // frames heard that failed their checks (FCS, Golay, Reed-Solomon)
    // details
    double timeSec = 0, inputRate = 0;
    bool carrier = false;
    uint64_t perFraming[cubesat::kNumFramings] = {};
    uint64_t rsFixed = 0;               // frames the RS code put right (one byte error or more)
    uint64_t kissSent = 0;
    double lastFrameSec = -1e9;
    double dialMhz = 0;                 // 0: unknown (a file)
    std::string tracked;                // the satellite the pass planner follows ("" none)
    int trackedNorad = 0;
    std::string trackedSetup;           // what the gr-satellites table set up for the tracked satellite ("USP, FSK 9600 baud, ...", "" none)
    std::vector<CubeFrame> frames;      // newest first
};

inline std::string cubesatSummary(const CubeTelemetry& t) {
    char b[160];
    if (!t.frames.empty() && t.dataValid) {
        const CubeFrame& f = t.frames.front();
        const std::string who = !f.sat.empty() ? f.sat : !f.from.empty() ? f.from : std::string("unknown");
        snprintf(b, sizeof b, "%llu frames, last %s (%s)", (unsigned long long)t.blocksOk, who.c_str(), cubesat::framingShort(f.framing));
        return b;
    }
    if (t.state >= 1) { snprintf(b, sizeof b, "signal, no frame yet%s", t.blocksBad ? " (some failed their checks)" : ""); return b; }
    snprintf(b, sizeof b, "%s", t.tracked.empty() ? "listening" : ("waiting for " + t.tracked).c_str());
    return b;
}

} // namespace dect2
