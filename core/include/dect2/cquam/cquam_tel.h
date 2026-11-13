// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AM stereo (C-QUAM) receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr int kCquamPilotBins = 61;   // the Q channel spectrum shown: 0 .. 60 Hz in 1 Hz steps

struct CquamTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 no carrier, 1 carrier locked (mono), 2 stereo (the 25 Hz pilot found)
    double cfoHz = 0;            // how far the carrier is from the channel centre (the loop's frequency)
    float snrDb = 0;             // carrier over the noise in the audio bandwidth
    float levelDb = -200;        // carrier power, dBFS
    bool dataValid = false;      // carrier locked and the noise estimate is in
    uint64_t blocksOk = 0;       // pilot checks (half a second each) that found the pilot
    uint64_t blocksBad = 0;      // and those that did not
    // details
    bool stereo = false;         // the pilot is present (and the mode allows stereo)
    bool pilotSeen = false;      // a pilot is present even when stereo is forced off
    float pilotDb = -200;        // 25 Hz pilot over the Q channel around it, dB
    float pilotPct = 0;          // pilot amplitude as a percentage of the carrier (about 5 on a C-QUAM station)
    float lockQuality = 0;       // mean cos of the stereo phase (1 for a mono station)
    float leftDb = -200, rightDb = -200;   // audio level, dBFS rms over the last 100 ms
    float widthDb = -200;        // L-R over L+R, dB (separation of the programme; -200 mono)
    float modPct = 0;            // peak modulation of L+R in the last second, percent of the carrier
    int mode = 0;                // 0 automatic, 1 mono, 2 stereo forced
    double widthHz = 0;          // audio bandwidth of the channel filter
    double inputRate = 0;
    double timeSec = 0;
    float qSpecDb[kCquamPilotBins] = {};   // Q channel (L-R and the pilot) level per Hz, dB relative to the carrier, 0 .. 60 Hz
    float noiseDb = -200;        // noise power in the audio bandwidth, dBFS (-200 not measured)
};

inline std::string cquamSummary(const CquamTelemetry& t) {
    char b[200];
    if (t.state == 0) return "AM stereo: no carrier yet";
    if (t.state == 1) { snprintf(b, sizeof b, "AM mono, carrier %+.1f Hz, C/N %.0f dB%s", t.cfoHz, t.snrDb, t.pilotSeen ? "" : ", no 25 Hz pilot"); return b; }
    snprintf(b, sizeof b, "C-QUAM stereo, carrier %+.1f Hz, C/N %.0f dB, pilot %.1f%%", t.cfoHz, t.snrDb, t.pilotPct);
    return b;
}

} // namespace dect2
