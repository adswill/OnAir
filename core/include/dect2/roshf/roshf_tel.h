// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ROS (HF) telemetry, shared by the receiver and the UI. The first members are copied into RxTelemetry (history plot, top bar).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kRoshfWaterRows = 90;       // rows kept for the waterfall (newest first)
constexpr size_t kRoshfWaterBins = 70;       // 50 Hz each, 0 .. 3500 Hz of the sideband audio
constexpr double kRoshfBinHz = 50.0;
constexpr size_t kRoshfEventsMax = 100;

// One detection report (every few seconds while a ROS-like signal is there)
struct RoshfEvent {
    int64_t utc = 0;
    double timeSec = 0;
    std::string subId, name, why;
    double baud = 0, spacingHz = 0, loHz = 0, hiHz = 0;   // tone range in the sideband audio
    double score = 0;
    bool weak = false;
};

struct RoshfTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 nothing, 1 a signal of about the right width, 2 ROS detected
    double cfoHz = 0;            // middle of the detected tone range relative to 1550 Hz (where a ROS signal is usually tuned)
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;      // a detection is current (nothing is ever decoded: see textStatus)
    uint64_t blocksOk = 0, blocksBad = 0;   // detections / measurements that did not fit
    double timeSec = 0, inputRate = 0;
    // the current detection (valid when state == 2)
    std::string subId, name, why;
    double baud = 0, spacingHz = 0, loHz = 0, hiHz = 0, spanHz = 0, score = 0;
    bool weak = false;
    int distinctTones = 0;
    // marker for the waterfall: the band the tones were seen in (audio Hz); 0 when none
    double markLoHz = 0, markHiHz = 0;
    std::string textStatus;      // "partial, needs a public spec": what is and is not read
    std::string text;            // always empty: the framing, coding and FEC of ROS are not public
    std::vector<std::vector<uint8_t>> water;   // newest first, kRoshfWaterBins values 0 .. 255 each
    std::vector<RoshfEvent> events;            // newest first
};

inline std::string roshfSummary(const RoshfTelemetry& t) {
    char b[160];
    if (t.state == 2) {
        std::snprintf(b, sizeof b, "%s%s, tones %.0f - %.0f Hz", t.name.c_str(), t.weak ? " (perhaps)" : "", t.loHz, t.hiHz);
        return b;
    }
    if (t.state == 1) return "ROS: a signal of about the right width, not identified";
    return "ROS: no signal";
}

} // namespace dect2
