// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// BLE tracker detector telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
// Privacy: a tracker is shown by its kind and the last two bytes of its address only, and forgotten after kBletrkForgetSec of silence.
#pragma once
#include "dect2/bletrk/bletrk_proto.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kBletrkHist = 24;       // levels of a tracker, one per 10 s, oldest first
constexpr size_t kBletrkMax = 200;       // trackers kept
constexpr size_t kBletrkAlertMax = 50;
constexpr double kBletrkForgetSec = 1800;   // an identifier is dropped after this long unheard

struct BletrkTracker {
    uint32_t id = 0;                     // for the interface only (not the address)
    std::string label;                   // "Find My ..:A5:58"
    int kind = 0;                        // bletrk::Kind
    bool separated = false;
    int status = -1;
    bool ignored = false;                // marked as the user's own
    float rssi = -200;                   // dBFS of the last packet (the radio's level, not calibrated dBm)
    float rssiAvg = -200, rssiPeak = -200;
    int trend = 0;                       // -1 moving away (weaker), 0 steady, +1 approaching
    int proximity = 0;                   // 0 far, 1 medium, 2 near (from the averaged level)
    float nearFrac = 0;                  // share of the seconds heard in which it was near
    float presence = 0;                  // share of the minutes between first and last heard in which it was heard
    std::vector<float> rssiHist;
    double firstSec = 0, lastSec = 0;    // signal time
    int64_t firstWall = 0, lastWall = 0;
    uint64_t packets = 0;
    int addresses = 1;                   // rotating addresses followed as this one tracker
    bool following = false;              // stays with the receiver: long enough, mostly heard, mostly near
    bool alerted = false;
};

struct BletrkAlert {
    int64_t wallTime = 0;
    double sec = 0;
    int kind = 0;
    double minutes = 0;
    float rssiAvg = -200;
    std::string text;
};

struct BletrkTelemetry {
    uint64_t seq = 0;
    int state = 0;                       // 0 nothing heard, 1 BLE packets heard in the last seconds
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;                // input level, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;               // BLE packets with a good CRC
    uint64_t blocksBad = 0;
    // details
    double timeSec = 0, inputRate = 0;
    int channel = 0;
    double alertSec = 600;               // how long a tracker must stay before the alert
    float nearDbfs = -60;                // averaged level counted as near
    uint64_t trackerPackets = 0;         // packets that were tracker advertisements
    int nowByKind[bletrk::kKinds] = {};      // distinct trackers (rotating addresses) heard in the last minute
    int windowByKind[bletrk::kKinds] = {};   // distinct trackers heard in the short memory window
    int followingNow = 0;
    std::vector<BletrkTracker> trackers; // following first, then the longest staying
    std::vector<BletrkAlert> alerts;     // newest first
};

inline std::string bletrkSummary(const BletrkTelemetry& t) {
    char b[160];
    if (t.blocksOk == 0) { if (t.channel) { snprintf(b, sizeof b, "listening on channel %d", t.channel); return b; } return "listening for advertising"; }
    int now = 0;
    for (int i = 1; i < bletrk::kKinds; i++) now += t.nowByKind[i];
    if (t.followingNow > 0) { snprintf(b, sizeof b, "%d tracker%s following you", t.followingNow, t.followingNow == 1 ? "" : "s"); return b; }
    if (now == 0) return "no trackers nearby";
    snprintf(b, sizeof b, "%d tracker%s nearby", now, now == 1 ? "" : "s");
    return b;
}

} // namespace dect2
