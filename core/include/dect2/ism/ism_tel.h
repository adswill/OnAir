// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ISM sensors receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include "ism_proto.h"
#include "ism_sigfox.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One device heard, keyed by model, ID and channel. Only what the device broadcasts is kept.
struct IsmDevice {
    std::string model, kind, id, channelName, mic;
    int channel = -1;
    int battery = -1;            // 1 ok, 0 low, -1 not sent
    std::vector<IsmField> fields;  // the last reading
    double freqHz = 0;           // where it was heard (absolute when the radio's frequency is known, else from the dial)
    float rssiDb = -200;         // burst power, dBFS
    float snrDb = 0;             // burst power over the noise floor
    bool fsk = false;
    int64_t firstWall = 0, lastWall = 0;   // Unix time
    double lastSec = 0;          // signal seconds since the start
    uint64_t count = 0;          // transmissions (repeats of one packet inside a burst, or within 2 s, count once)
    std::string histName, histUnit;        // the field the history follows (the first one with a unit)
    std::vector<float> hist;     // its last values, oldest first, at most kIsmHistMax
};

// One line of the live log
struct IsmEvent {
    int64_t wallTime = 0;
    double sec = 0;
    std::string model, id, text;   // text: the reading in one line, or the unknown signal's code
    double freqHz = 0;
    float rssiDb = -200;
    bool unknown = false;
};

// A burst no protocol accepted: kept with its timing so it can be reported
struct IsmRaw {
    int64_t wallTime = 0;
    double sec = 0;
    double freqHz = 0;
    float rssiDb = -200, snrDb = 0;
    bool fsk = false;
    float fskDevHz = 0;          // FSK: half the distance between the two tones
    IsmGuess guess;
    std::vector<IsmPulse> pulses;  // at most kIsmRawPulsesMax
};

struct IsmTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 no input yet, 1 listening, 2 a device was heard in the last 10 s
    double cfoHz = 0;            // always 0 (every burst is measured on its own)
    float snrDb = 0;             // of the last decoded burst
    float levelDb = -200;        // input power, dBFS
    bool dataValid = false;      // a device was heard in the last 10 s
    uint64_t blocksOk = 0;       // packets decoded
    uint64_t blocksBad = 0;      // bursts no protocol accepted
    // set-up and statistics
    double timeSec = 0;
    double inputRate = 0;
    double centerHz = 0;         // the radio's centre (0: unknown, frequencies are then offsets from the dial)
    double dialOffsetHz = 0;     // where the dial frequency sits in the input
    uint64_t bursts = 0;         // bursts detected (each may hold several signals)
    float noiseDb = -200;        // the noise floor per FFT bin, dBFS
    std::vector<IsmDevice> devices;   // most recently heard first
    std::vector<IsmEvent> events;     // newest first, at most kIsmLogMax
    std::vector<IsmRaw> raws;         // newest first, at most kIsmRawMax
    SigfoxTelemetry sigfox;           // Sigfox uplinks and the 192 kHz waterfall (active when the dial is on a Sigfox band)
};

constexpr size_t kIsmHistMax = 64;
constexpr size_t kIsmLogMax = 400;
constexpr size_t kIsmRawMax = 60;
constexpr size_t kIsmRawPulsesMax = 600;
constexpr size_t kIsmDevMax = 300;

inline std::string ismReadingText(const std::vector<IsmField>& f) {
    std::string s;
    for (const auto& x : f) {
        if (!s.empty()) s += ", ";
        char b[96];
        if (!x.text.empty()) snprintf(b, sizeof b, "%s %s", x.name.c_str(), x.text.c_str());
        else snprintf(b, sizeof b, "%s %.*f%s%s", x.name.c_str(), x.decimals, x.value, x.unit.empty() ? "" : " ", x.unit.c_str());
        s += b;
    }
    return s;
}

inline std::string ismSummary(const IsmTelemetry& t) {
    char b[160];
    if (t.state == 0) return "waiting for samples";
    if (t.sigfox.active && t.sigfox.frames) {
        snprintf(b, sizeof b, "%zu device%s, %llu packets, %zu Sigfox message%s", t.devices.size(), t.devices.size() == 1 ? "" : "s",
                 (unsigned long long)t.blocksOk, t.sigfox.messages.size(), t.sigfox.messages.size() == 1 ? "" : "s");
        return b;
    }
    snprintf(b, sizeof b, "%zu device%s, %llu packets, %llu unknown", t.devices.size(), t.devices.size() == 1 ? "" : "s",
             (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    return b;
}

} // namespace dect2
