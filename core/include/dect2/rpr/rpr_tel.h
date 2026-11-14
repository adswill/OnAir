// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Robust Packet (RPR) telemetry, shared by the receiver and the UI. The first members are copied into RxTelemetry (history plot, top bar).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kRprEventsMax = 60;
constexpr size_t kRprBurstsMax = 60;
constexpr size_t kRprHist = 120;
constexpr size_t kRprRowSyms = 200;

struct RprEvent {
    int64_t utc = 0;
    double timeSec = 0;
    std::string text;
};

struct RprBurst {
    int64_t utc = 0;
    double timeSec = 0, lenSec = 0, snrDb = 0;
    int level = 0, order = 0, nSyms = 0;
    std::string bits;        // the first demodulated bits (differential, still coded and scrambled), hex
};

struct RprTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 nothing, 1 carrier comb found, 2 bursts demodulated
    double cfoHz = 0;            // comb centre relative to 1500 Hz
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;      // at least one burst demodulated
    uint64_t blocksOk = 0;       // bursts demodulated
    uint64_t blocksBad = 0;
    double centreHz = 0, combDb = 0;
    int carriersSeen = 0, level = 0;
    std::vector<double> carrierHz;
    std::vector<std::string> rows;   // the newest symbols of the first carriers
    uint64_t analyses = 0, detections = 0;
    double timeSec = 0, inputRate = 0;
    std::vector<RprBurst> bursts;    // newest first
    std::vector<RprEvent> events;    // newest first
    std::vector<float> levelHist;
};

inline std::string rprSummary(const RprTelemetry& t) {
    char b[200];
    if (t.state == 0) return "Robust Packet: no signal";
    if (t.level) snprintf(b, sizeof b, "Robust Packet: 8 carriers, speed level %d (%s), %llu bursts (frames not decoded, needs a recording)", t.level, t.level == 1 ? "D-BPSK" : "D-QPSK", (unsigned long long)t.blocksOk);
    else snprintf(b, sizeof b, "Robust Packet: 8 carrier comb at %.0f Hz, waiting for a burst", t.centreHz);
    return b;
}

} // namespace dect2
