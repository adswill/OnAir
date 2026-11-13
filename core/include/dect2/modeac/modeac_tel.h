// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode A / C receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include "dect2/adsb/adsb_tel.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kModeAcMax = 128;       // codes kept in the table

// One code on the air: a reply pattern that was heard (at least twice) lately
struct ModeAcCode {
    uint16_t bits = 0;                   // the thirteen information positions (modeac_proto.h)
    int squawk = 0;                      // read as Mode A, written like the digits: 7700
    bool altValid = false; int altFt = 0;   // read as Mode C (Gillham code), when the pattern is one
    int kind = 0;                        // 0 Mode A or C (cannot tell), 1 Mode A, 2 Mode C
    int how = 0;                         // why: 0 -, 1 not a Gillham code, 2 emergency or standard squawk, 3 same squawk as a Mode S aircraft, 4 altitude agrees with one aircraft
    bool spi = false;                    // the ident pulse was seen in the last reply
    uint32_t count = 0;                  // replies with this code
    float levelDbfs = -120, snrDb = 0;   // of the last replies
    float ageSec = 0;                    // since the last one
    float firstAgeSec = 0;               // since the first one
    uint32_t icao = 0;                   // the Mode S aircraft this code was matched to (0: none)
    std::string callsign;
};

struct ModeAcTelemetry {
    // ---- the part every mode has: the engine copies it into RxTelemetry; keep these members and what they mean
    uint64_t seq = 0;            // grows with every report and never restarts
    int state = 0;               // 0 searching (nothing for 10 s), 1 Mode S heard but no Mode A/C reply lately, 2 receiving replies
    double cfoHz = 0;            // not measured (the receiver works on the pulse envelope): always 0
    float snrDb = 0;             // mean signal-to-noise ratio of the last accepted replies
    bool dataValid = false;      // replies are coming out
    uint64_t blocksOk = 0, blocksBad = 0;   // replies accepted / candidates with the right F1 - F2 spacing that failed the shape test, since the start
    // ---- this mode's own fields below
    double timeSec = 0;
    float repliesPerSec = 0;     // accepted replies per second, over the last 5 seconds
    float noiseDbfs = -120;
    float levelDbfs = -120;
    uint64_t candidates = 0;     // F1 - F2 pairs examined
    uint64_t masked = 0;         // thrown away because a Mode S message was on the air there
    uint64_t pending = 0;        // replies seen once and waiting for a second sighting
    uint64_t modeSGood = 0;      // Mode S messages decoded by the shared ADS-B front end
    uint32_t codeCount = 0, matched = 0;
    std::vector<ModeAcCode> codes;       // most recently heard first
    AdsbTelemetry adsb;                  // the aircraft the same capture shows as Mode S / ADS-B: the map and the matching use them
};

inline std::string modeAcSummary(const ModeAcTelemetry& t) {
    char b[200];
    snprintf(b, sizeof b, "Mode A/C: %u codes (%u matched to a Mode S aircraft)  %.1f replies/s  SNR %.1f dB  Mode S %u aircraft",
             t.codeCount, t.matched, (double)t.repliesPerSec, (double)t.snrDb, t.adsb.aircraftCount);
    return b;
}

} // namespace dect2
