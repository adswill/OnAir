// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Project Horus receiver telemetry: RxTelemetry::horus, active standard 157 (family 156, engine standard 158), poll slot 157.
#pragma once
#include "dect2/horus/horus_modes.h"
#include "dect2/horus/horus_proto.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct HorusTrackPoint { double unixT = 0, lat = 0, lon = 0; float altM = 0; };

// one payload (balloon) heard: by callsign, or "ID <n>" when the ID table does not know it
struct HorusPayload {
    std::string callsign;
    int payloadId = -1;
    horus::Packet last;
    std::vector<HorusTrackPoint> track;    // oldest first, at most 3000 points
    uint64_t packets = 0;
    double firstHeard = 0, lastHeard = 0;  // Unix seconds
    double maxAltM = 0, ascentMs = 0;      // the climb rate over the last minute or so
    bool hasAscent = false, active = false;
    double lastHeardS = 0;                 // seconds since
};

// one demodulator channel: the dial channel (0) and the ones the wide-span search opened
struct HorusChannelInfo {
    double offsetHz = 0;                   // the channel's centre from the dial
    double tones[4] = {}; int ntones = 0;  // the 4FSK tones it follows, relative to the channel centre
    double rtty[2] = {};
    double spacingHz = 0;
    float snrDb = -99;
    int mode = 0;                          // 0 nothing decoded yet, 1 Horus Binary, 2 RTTY
    uint64_t packets = 0, syncs = 0, failures = 0;
    bool dynamic = false;
};

struct HorusTelemetry {
    uint64_t seq = 0, blocksOk = 0, blocksBad = 0, syncs = 0;
    int state = 0;                         // 0 waiting for samples, 1 listening, 2 decoding (a packet in the last 30 s)
    double inputRate = 0, centerHz = 0, timeSec = 0, cfoHz = 0;
    float snrDb = 0, levelDb = -200;
    bool dataValid = false;
    int maxChannels = 6;
    bool wide = true;
    std::vector<HorusPayload> payloads;    // most recently heard first
    std::vector<horus::Packet> packets;    // most recent first, at most 200
    std::vector<HorusChannelInfo> channels;
    std::vector<float> spectrumDb;         // the dial channel's averaged spectrum (the tone estimator's view), dB over its floor
    double specLoHz = 0, specStepHz = 0;   // frequency of spectrumDb[0] from the dial, and the step
};

inline std::string horusSummary(const HorusTelemetry& t) {
    if (t.state == 0) return "Horus: waiting for samples";
    if (t.payloads.empty()) return "Horus: listening";
    const auto& p = t.payloads.front();
    char b[160];
    snprintf(b, sizeof b, "Horus %s #%u %.5f %.5f %.0f m", p.callsign.c_str(), p.last.seq, p.last.lat, p.last.lon, p.last.altM);
    return b;
}

} // namespace dect2
