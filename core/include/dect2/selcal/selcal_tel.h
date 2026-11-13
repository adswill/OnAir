// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SELCAL telemetry. RxTelemetry::selcal, engine standard 242, active standard 241 (family 240).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kSelcalCallsMax = 300;

// One SELCAL call: two pairs of tones, e.g. "AM-CP" (ARINC 714 plan, 16 tones, 312.6 - 1479.1 Hz)
struct SelcalCall {
    uint64_t serial = 0;
    int64_t utc = 0;             // computer clock, UTC seconds
    double timeSec = 0;          // signal seconds
    std::string code;            // "AM-CP"
    double hz[4] = {};           // the four tones: pair 1 low / high, pair 2 low / high
    double dialHz = 0;           // the tuned frequency (0 = not known: a file or a sound card)
    float levelDb = -200;        // audio level during the call, 0 dB = 5 kHz deviation (or full scale for audio input)
    std::string aircraft;        // registration / callsign from selcal_aircraft.tsv in the data folder; "" when not listed
};

// One code heard, however many times
struct SelcalCode {
    std::string code, aircraft;
    uint64_t count = 0;
    int64_t firstSeen = 0, lastSeen = 0;
    double lastDialHz = 0;
};

struct SelcalTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 nothing, 1 audio present, 2 a call decoded in the last minute
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;
    double timeSec = 0, inputRate = 0;
    double dialHz = 0;
    int demod = 0;               // TonesDemod of the radio's channel
    bool audioInput = false;
    float audioDb = -200;
    double toneHz = 0;           // the strongest pure tone now
    float toneDb = -200;
    std::vector<float> spec;     // audio spectrum 0 .. 3.5 kHz, dB
    std::vector<SelcalCall> calls;     // newest first
    std::vector<SelcalCode> codes;     // newest first
};

inline std::string selcalSummary(const SelcalTelemetry& t) {
    char b[160];
    if (!t.calls.empty()) { snprintf(b, sizeof b, "SELCAL %s (%llu call%s, %zu code%s)", t.calls[0].code.c_str(), (unsigned long long)t.blocksOk, t.blocksOk == 1 ? "" : "s", t.codes.size(), t.codes.size() == 1 ? "" : "s"); return b; }
    return t.state ? "SELCAL: listening" : "SELCAL: no audio";
}

} // namespace dect2
