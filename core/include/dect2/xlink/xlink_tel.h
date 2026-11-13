// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Telemetry of the extra-links family, shared by the receiver and the UI.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct XlinkFrame {
    int64_t utc = 0;
    double timeSec = 0;
    int proto = 0;
    std::string id, type, detail, hex;
    double freqHz = 0;
    float snrDb = 0;
};

struct XlinkLink {
    int proto = 0;
    std::string id, lastType, lastDetail;
    uint64_t count = 0, lost = 0;
    int64_t firstSeen = 0, lastSeen = 0;
    float lastSnr = 0;
};

struct XlinkTelemetry {
    uint64_t seq = 0;
    int state = 0;           // 0 nothing, 1 PHY activity, 2 frames lately
    double cfoHz = 0;
    float snrDb = 0, levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;
    double timeSec = 0, inputRate = 0;
    int protocols = 0;
    bool rateTooLow = false; // the capture is slower than this protocol's channel
    uint64_t phyFrames = 0, phyUnsupported = 0;
    std::vector<XlinkFrame> frames;     // newest first
    std::vector<XlinkLink> links;       // newest first
};

inline std::string xlinkSummary(const XlinkTelemetry& t) {
    char b[160];
    if (t.rateTooLow) return "Needs a sample rate of 20 Msps or more for a 20 MHz channel";
    if (t.blocksOk) { std::snprintf(b, sizeof b, "%llu frames from %zu link(s)", (unsigned long long)t.blocksOk, t.links.size()); return b; }
    if (t.phyFrames) { std::snprintf(b, sizeof b, "%llu 802.11n frames on air, none from these links", (unsigned long long)t.phyFrames); return b; }
    return "No signal";
}

} // namespace dect2
