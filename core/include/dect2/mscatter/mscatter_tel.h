// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteor scatter detector telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kMsPingMax = 500;      // pings kept in the list
constexpr int kMsWfCols = 86;           // waterfall: columns across +-500 Hz around the beacon
constexpr int kMsWfRows = 60;           // waterfall: rows, newest last
constexpr int kMsProfile = 32;          // points of a ping's level profile

struct MsPing {
    uint32_t id = 0;
    int64_t wallTime = 0;               // UTC seconds of the start
    float durationSec = 0;
    float peakSnrDb = 0;                // strongest bin over its own long-term level
    float dopplerHz = 0;                // at the peak, from the beacon line
    float driftHzPerSec = 0;            // slope of the Doppler over the ping (head-echo / wind shear hint)
    int kind = 0;                       // 0 underdense (sharp rise, exponential decay), 1 overdense (long, flat top)
    std::vector<float> profile;         // peak level per slice of the ping (dB), at most kMsProfile points
};

// A satellite seen in the GRAVES radar's reflection: much longer than a meteor ping, with a Doppler that sweeps (see core/src/satmisc)
struct MsSatEcho {
    uint32_t id = 0;
    int64_t wallTime = 0;               // UTC seconds of the start
    float durationSec = 0;
    float peakSnrDb = 0;
    float startHz = 0, endHz = 0;       // Doppler from the beacon line
    std::string name;                   // the best matching satellite with known elements; "" when none matched
    int norad = 0;
    float rmsHz = 0;                    // residual of the matching satellite's Doppler curve
    bool ambiguous = false;             // several satellites fit equally well
};
constexpr size_t kMsSatEchoMax = 100;

struct MsShower {
    std::string name;
    int peakDayDelta = 0;               // days from today to the peak (negative: past)
    int zhr = 0;
    bool active = false;
    bool daytime = false;               // radiant is up in daylight: invisible to the eye, strong for radio
};

struct MsTelemetry {
    uint64_t seq = 0;
    int state = 0;                      // 0 no beacon, 1 beacon found, 2 pings counted
    double cfoHz = 0;                   // the beacon line relative to the radio's centre
    float snrDb = 0;                    // beacon over the noise in one bin
    float levelDb = -200;               // beacon level, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // pings counted, bursts rejected as broadband interference
    // details
    double timeSec = 0, inputRate = 0, workRate = 0, binHz = 0, centreHz = 0;
    float noiseDb = -200;               // dBFS in one bin
    float thresholdDb = 13;
    uint64_t rejected = 0;
    bool active = false;                // a ping is in progress
    float activeSnrDb = 0;
    std::vector<MsPing> pings;          // newest first
    std::vector<MsSatEcho> satEchoes;   // newest first
    uint64_t satEchoTotal = 0, satEchoMatched = 0;
    int satCatalog = 0;                 // satellites with elements given to setSatelliteMatch
    std::vector<int> hourly;            // last 24 hours, UTC hours, the current one last
    std::vector<int> daily;             // last 30 days, UTC days, today last
    int64_t hourStart = 0, dayStart = 0;   // UTC seconds of the start of the last hourly / daily bin
    float ratePerHour = 0;              // pings in the last 60 minutes
    std::vector<MsShower> showers;      // around today (see msShowers)
    std::vector<uint8_t> waterfall;     // kMsWfRows x kMsWfCols, 0 = background, 255 = 30 dB; row 0 oldest
    std::vector<float> spectrum;        // kMsWfCols: the long-term spectrum around the beacon, dB over noise
    bool logging = false;
    std::string logPath;
};

inline std::string msSummary(const MsTelemetry& t) {
    char b[160];
    if (t.state == 0) return "Meteor scatter: no beacon in the capture";
    snprintf(b, sizeof b, "Meteor scatter: %llu pings, %.0f per hour", (unsigned long long)t.blocksOk, (double)t.ratePerHour);
    return b;
}

} // namespace dect2
