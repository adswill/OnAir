// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sigfox mode telemetry. RxTelemetry::sigfoxm, engine standard 155, active standard 154 (family 153). Receive only.
#pragma once
#include "dect2/ism/ism_sigfox.h"
#include <cstdint>
#include <cstdio>
#include <string>

namespace dect2 {

struct SigfoxModeTelemetry {
    uint64_t seq = 0;
    int state = 0;                 // 0 no input (or the sample rate is too low), 1 listening, 2 an uplink decoded in the last 5 minutes
    double inputRate = 0, dialHz = 0, timeSec = 0;
    std::string zone;              // the regional profile of the dial ("RC1" ...), empty when the dial is on none
    int zoneBaud = 0;              // the bit rate that profile uses (100 or 600; the receiver tries both anyway)
    float levelDb = -200;          // input level, dBFS
    double cfoHz = 0;              // the last uplink's carrier, Hz from the dial
    float snrDb = 0;               // not measured per uplink (0)
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // repetitions decoded / narrow carriers that did not decode
    SigfoxTelemetry sf;           // messages (newest first), waterfall, counters
};

inline std::string sigfoxModeSummary(const SigfoxModeTelemetry& t) {
    char b[200];
    if (t.state == 0) return "Sigfox: waiting for samples (needs 400 ksps or more)";
    if (t.sf.messages.empty()) {
        snprintf(b, sizeof b, "Sigfox: listening%s%s, %llu carriers", t.zone.empty() ? "" : " on ", t.zone.c_str(), (unsigned long long)t.sf.carriers);
        return b;
    }
    const SigfoxMessage& m = t.sf.messages.front();
    snprintf(b, sizeof b, "Sigfox %s #%u %s", m.id.c_str(), m.seq, m.payload.empty() ? (m.singleBit ? (m.bitValue ? "bit 1" : "bit 0") : "(no payload)") : m.payload.c_str());
    return b;
}

}  // namespace dect2
