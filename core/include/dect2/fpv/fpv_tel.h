// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Analog FPV video receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include "dect2/atv/atv_tel.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kFpvRssiHist = 120;

// One channel of the plans (fpv_chan.h) with what was last measured on it. A channel that never lay in the capture has levelDb -200.
struct FpvChannelInfo {
    std::string name;            // "R5"
    int band = 0, ch = 0;
    double freqHz = 0;
    float levelDb = -200;        // power in the channel, dBFS of the capture it was last measured in
    float overDb = 0;            // above the noise floor of that capture
    bool present = false;        // a transmitter: overDb above the threshold
    bool inCapture = false;      // inside the capture now
    double ageSec = -1;          // seconds since it was last measured; -1 never
};

struct FpvTelemetry {
    // ---- the part every mode has
    uint64_t seq = 0;
    int state = 0;               // 0 no transmitter, 1 a transmitter on the tuned channel (carrier), 2 video locked and decoding
    double cfoHz = 0;            // the transmitter's distance from the nominal channel frequency
    float snrDb = 0;             // video signal-to-noise ratio of the picture decoder
    float levelDb = -200;        // power of the tuned channel, dBFS
    bool dataValid = false;      // a picture is coming out
    uint64_t blocksOk = 0, blocksBad = 0;   // fields decoded whole / damaged

    // ---- this mode's own fields
    double inputRate = 0, centreHz = 0;
    double tunedHz = 0;          // the channel taken to the picture decoder
    std::string tunedName;       // "R5", or "" when the centre is not on a channel
    float noiseFloorDb = -200;   // the capture's noise in the width of a channel, dBFS
    float overDb = 0;            // the tuned channel above that
    std::vector<FpvChannelInfo> channels;   // all channels, in the order of fpv_chan.h
    AtvTelemetry video;          // the picture decoder: system, lines, sync, scopes
    // the sound subcarrier (decoded only while sound is on)
    bool audioOn = false;
    bool audioPresent = false;
    double audioSubHz = 0;       // the subcarrier in use, from the video carrier
    float audioLevelDb = -120;
    float audioSubDb[2] = {-120, -120};   // strength of the 6.0 and 6.5 MHz subcarriers above the noise beside them
    std::vector<float> spec;     // the capture's spectrum, dB, centred (for the screen)
    float specSpanMhz = 0;
    std::vector<float> rssiHist; // tuned channel level, one point a second
};

inline std::string fpvSummary(const FpvTelemetry& t) {
    char b[200];
    const char* ch = t.tunedName.empty() ? "" : t.tunedName.c_str();
    if (t.state == 2) {
        snprintf(b, sizeof b, "FPV video %s%s%.0f MHz: %s, video SNR %.1f dB, %.0f dB over noise", ch, ch[0] ? " " : "", t.tunedHz / 1e6, t.video.system.c_str(), t.snrDb, t.overDb);
        return b;
    }
    if (t.state == 1) { snprintf(b, sizeof b, "FPV video %s%s%.0f MHz: transmitter found, %.0f dB over noise, waiting for sync", ch, ch[0] ? " " : "", t.tunedHz / 1e6, t.overDb); return b; }
    return "FPV video: no transmitter on this channel";
}

} // namespace dect2
