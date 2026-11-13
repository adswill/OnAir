// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRa satellite receiver telemetry, shared by the receiver and the UI. The first members are copied into RxTelemetry (top bar, history plot): keep them.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kLsLogMax = 400;
constexpr size_t kLsTrackMax = 600;

struct LoraSatSettings {
    int preset = -1;               // lorasat::presets() index shown in the UI (-1 custom); informational, the values below are what runs
    bool followDial = true;        // when the dial is within 15 kHz of a preset, run that preset's settings (spreading factor, bandwidth, sync word)
    bool autoSf = true;            // search SF7 - SF12 (a preset that names an SF overrides it while followDial is on)
    int sf = 11;
    bool autoBw = false;           // search 125, 250 and 500 kHz
    int bwKhz = 125;               // 62 (62.5), 125, 250, 500
    int sync = -1;                 // sync word, -1: try 0x12 and 0x34
    int preamble = 8;
    double dopplerMaxHz = 0;       // the widest Doppler shift the channel filter passes, either way; 0: from the dial frequency (LEO at 600 km)
    bool track = true;             // follow the carrier through each frame (Doppler rate); off: one measurement per frame, as for a ground link
    double offsetHz = 0;           // the channel's offset from the dial
};

struct LsFrame {
    int64_t wallTime = 0;
    double sec = 0;
    int sf = 0;
    double bwHz = 0;
    int cr = 0;
    bool hasCrc = false, crcOk = false;
    int sync = 0;
    int len = 0;
    int corrected = 0;
    float snrDb = 0, levelDb = 0;
    double cfoHz = 0;              // carrier offset from the dial at the end of the frame
    double cfoStartHz = 0;         // at the first data symbol
    double rateHzS = 0;            // drift of the carrier during the frame, Hz per second (the Doppler rate seen through the frame)
    double freqHz = 0;             // dial + offset at the start of the data (absolute when the radio's centre is known)
    std::string sat;               // the satellite the pass planner said is overhead ("" none)
    std::string hex, ascii;
};

struct LsDecoderInfo {
    int sf = 0;
    double bwHz = 0;
    int sync = 0;
    bool inBand = false;
    uint64_t preambles = 0, frames = 0, crcBad = 0, headerBad = 0, syncBad = 0;
};

struct LsDopplerPoint {
    float sec = 0;                 // signal time
    float hz = 0;                  // carrier offset from the dial at the frame start
    float rate = 0;                // Hz per second
};

struct LoraSatTelemetry {
    uint64_t seq = 0;
    int state = 0;                 // 0 nothing heard, 1 frames heard in the last seconds
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;
    // details
    double timeSec = 0, inputRate = 0, centerHz = 0, dialHz = 0;
    bool centreKnown = false;
    LoraSatSettings cfg;
    int presetRunning = -1;        // the preset followDial matched, -1 none
    double dopplerMaxHz = 0;       // the filter's allowance in use
    double filterHz = 0;
    uint64_t preambles = 0, headerBad = 0;
    uint64_t perSf[13] = {};
    std::string tracked;           // satellite name from the pass planner
    int trackedNorad = 0;
    std::vector<LsDecoderInfo> decoders;
    std::vector<LsFrame> log;      // newest first
    std::vector<LsDopplerPoint> doppler;   // oldest first: the carrier offset of each good frame, for the pass plot
};

inline std::string loraSatSummary(const LoraSatTelemetry& t) {
    char b[160];
    if (t.blocksOk + t.blocksBad == 0) {
        size_t n = 0;
        for (const auto& d : t.decoders) if (d.inBand) n++;
        snprintf(b, sizeof b, "listening, %zu demodulators, Doppler up to +-%.1f kHz%s%s", n, t.dopplerMaxHz / 1e3, t.tracked.empty() ? "" : ", pass of ", t.tracked.c_str());
        return b;
    }
    snprintf(b, sizeof b, "%llu frames, %llu CRC errors%s%s", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.tracked.empty() ? "" : ", pass of ", t.tracked.c_str());
    return b;
}

} // namespace dect2
