// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CODAR / HF ocean radar monitor telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kCdStationMax = 64;
constexpr size_t kCdEventMax = 300;
constexpr size_t kCdSnrHist = 120;      // points of SNR history per station (one per second heard)
constexpr size_t kCdSweepMarks = 80;    // sweeps shown as marks on the waterfall
constexpr int kCdWfCols = 256;
constexpr int kCdWfRows = 160;

// One radar (a cluster of sweeps with the same centre, width and repetition)
struct CdStation {
    int id = 0;                         // 1, 2, ... in the order heard
    double centreMhz = 0;               // absolute (radio centre + offset); 0 when the radio's frequency is not known (a file)
    double offsetKhz = 0;               // from the radio's centre
    double bwKhz = 0;                   // sweep width
    double rateKhzS = 0;                // sweep rate, kHz per second
    double periodSec = 0;               // sweep repetition (0: not known yet)
    int dir = 0;                        // +1 the frequency rises during a sweep, -1 it falls
    uint64_t sweeps = 0;
    float snrDb = 0, snrMax = -100;     // bin SNR of the last sweep / best
    float levelDb = -200;               // dBFS of the sweep's peak bin
    std::vector<float> snrHist;         // one point per second heard, oldest first
    double firstSec = 0, lastSec = 0;   // signal time
    int64_t firstWall = 0, lastWall = 0;
    std::string band;                   // the radar band it is in ("12 - 14 MHz") or "outside the radar bands"
    std::string hint;                   // "possible ...": only a guess from the sweep numbers
    std::string callsign;               // decoded from the keyed energy, "" none
    bool idUnverified = true;           // always: built from the published description, not checked against a recording
    double idWpm = 0;
    int idSeen = 0;                     // how many decodes gave this text
    int64_t idWall = 0;
};

// "Heard on band X at time Y": a station first heard, or heard again after a pause (propagation history, no position is claimed)
struct CdEvent {
    int64_t wall = 0;
    int station = 0;
    std::string band;
    double centreMhz = 0;
    float snrDb = 0;
};

struct CdSweep {
    double sec = 0;                     // signal time of the sweep's start
    float f0Khz = 0, f1Khz = 0;         // offsets from the radio's centre at the start and at the end
    int station = 0;
};

struct CodarTelemetry {
    uint64_t seq = 0;
    int state = 0;                      // 0 nothing heard, 1 sweeps heard in the last seconds
    double cfoHz = 0;                   // unused
    float snrDb = 0;                    // best SNR of the last sweeps
    float levelDb = -200;               // input level, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;              // sweeps accepted
    uint64_t blocksBad = 0;             // tracks rejected (not linear, too wide, too short)
    // details
    double timeSec = 0, inputRate = 0;
    double centerMhz = 0;               // where the radio is tuned (0: not known)
    double spanKhz = 0;                 // what the waterfall covers
    std::string bandNow;                // the radar band the radio is in, "" none
    uint64_t wideBursts = 0;            // frames skipped as broadband bursts
    std::vector<CdStation> stations;    // most recently heard first
    std::vector<CdEvent> events;        // newest first
    std::vector<CdSweep> sweepMarks;    // newest last
    std::vector<uint8_t> wf;            // kCdWfRows x kCdWfCols, oldest row first: 0 .. 255 = 0 .. 30 dB above the noise floor
    double wfRowSec = 0.025, wfEndSec = 0;   // seconds per row; the signal time at the end of the newest row
};

inline std::string codarSummary(const CodarTelemetry& t) {
    char b[160];
    if (t.stations.empty()) {
        if (t.centerMhz > 0) { snprintf(b, sizeof b, "listening on %.3f MHz%s%s", t.centerMhz, t.bandNow.empty() ? "" : ", ", t.bandNow.c_str()); return b; }
        return "listening for ocean radar sweeps";
    }
    const CdStation& s = t.stations.front();
    snprintf(b, sizeof b, "%zu radar%s, %.0f kHz sweeps%s%s", t.stations.size(), t.stations.size() == 1 ? "" : "s", s.bwKhz, s.callsign.empty() ? "" : ", ID ", s.callsign.c_str());
    return b;
}

} // namespace dect2
