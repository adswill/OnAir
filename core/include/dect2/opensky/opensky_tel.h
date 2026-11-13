// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OpenSky signal identifier telemetry. RxTelemetry::opensky, engine standard 324, active standard 323 (family 322). Receive only, no decoding: OpenSky's air interface is not published and the traffic is encrypted.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct OpenSkyTelemetry {
    uint64_t seq = 0;
    int state = 0;                  // 0 no input, 1 analysing, 2 result
    bool locked = false;            // a confident match
    bool dataValid = false;
    float levelDb = -200, snrDb = 0;
    double cfoHz = 0;               // not measured separately
    uint64_t blocksOk = 0, blocksBad = 0;
    double timeSec = 0, inputRate = 0;
    double baud = 0;                // the symbol rate seen in the spectrum of the frequency transitions, 0 none
    float lineDb = 0;               // how far that line stands above the floor
    int levels = 0;                 // frequency levels seen: 2, 4, 0 unclear
    float deviationHz = 0;          // spread of the level centres
    double burstPeriodMs = 0;       // the envelope's repetition period (TDMA framing), 0 continuous
    float burstDuty = 0;            // share of the time the carrier is on (0..1)
    float occupiedHz = 0;           // width holding 90 % of the power
    bool looksLikeOpenSky = false;  // 9600 baud +- 3 %, four levels
    std::string verdict;
};

inline std::string openSkySummary(const OpenSkyTelemetry& t) {
    char b[160];
    if (t.state == 0) return "OpenSky: waiting for samples";
    if (!t.dataValid) return "OpenSky: analysing";
    snprintf(b, sizeof b, "%s: %.0f baud, %d levels", t.looksLikeOpenSky ? "OpenSky-like signal" : "Not OpenSky", t.baud, t.levels);
    return b;
}

} // namespace dect2
