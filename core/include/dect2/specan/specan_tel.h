// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct SpecanTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 waiting for the first sweep, 1 sweeping, 2 sweeps coming
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;        // the strongest bin of the last sweep
    bool dataValid = false;
    uint64_t blocksOk = 0;       // sweeps finished
    uint64_t blocksBad = 0;      // sweeps with gaps (bins no step covered)
    double inputRate = 0;
    bool native = false;         // the HackRF's own sweep (else stepped retuning)
    int steps = 0;               // tuning steps in the last sweep
    float sweepMs = 0;           // the last sweep took this long
    double loHz = 0, binHz = 0;  // the grid: bin k is loHz + (k + 0.5) binHz
    double peakHz = 0;           // where levelDb is
    std::vector<float> live, peak, avg;   // dBFS per bin
    std::vector<double> stepLos; // the radio's centres in the last sweep
};

inline std::string specanSummary(const SpecanTelemetry& t) {
    char b[200];
    if (!t.blocksOk) snprintf(b, sizeof b, "sweeping (%s)", t.native ? "HackRF sweep" : "stepped");
    else snprintf(b, sizeof b, "%llu sweeps, %.0f ms each, peak %.1f dBFS at %.3f MHz", (unsigned long long)t.blocksOk, t.sweepMs, t.levelDb, t.peakHz / 1e6);
    return b;
}

} // namespace dect2
