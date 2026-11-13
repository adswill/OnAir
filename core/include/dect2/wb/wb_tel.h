// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal workbench: the live part of the mode is only a level meter on the radio's samples (the tool itself works on files, see wb_*.h).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace dect2 {

struct WbTelemetry {
    uint64_t seq = 0;
    int state = 0;                 // 0 no samples, 1 samples arriving
    double cfoHz = 0;              // unused
    float snrDb = 0;               // unused
    float levelDb = -200;          // input level, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;
    double timeSec = 0, inputRate = 0;
};

inline std::string wbSummary(const WbTelemetry& t) {
    char b[96];
    if (t.state == 0) return "open a recording to analyse it";
    snprintf(b, sizeof b, "live input %.1f dBFS at %.3f Msps", t.levelDb, t.inputRate / 1e6);
    return b;
}

} // namespace dect2
