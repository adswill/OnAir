// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// What the Orbcomm receiver reports (RxTelemetry::orbcomm, standard 45).
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include "orbcomm.h"
#include "dect2/common/ring.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr int kOrbcommChannels = 2;     // downlinks decoded at once
constexpr size_t kOrbcommLogKeep = 500;  // packets kept in the log (sync and fill are only counted)

struct OrbcommPacket {
    double time = 0;             // signal time (s)
    int sat = -1;                // from the frame's sync packet, -1 before one was seen
    int chan = 0;                // decoder 0 or 1
    int type = 0;                // orbcomm::PktType
    uint8_t bytes[orbcomm::kPktBytes] = {};
    std::string text;            // orbcomm::describe, or the whole message once its last part came
};

struct OrbcommSat {
    int id = -1;
    double lastHeard = 0;        // signal time
    double freqHz = 0;           // carrier, absolute when the tuned frequency is known (else the offset from the centre)
    double nominalHz = 0;        // the raster channel it sits on
    double dopplerHz = 0;        // carrier minus the raster channel
    float snrDb = 0;
    int chan = -1;               // decoder hearing it now, -1 none
    uint64_t frames = 0, packets = 0, bad = 0;
    bool hasPos = false;
    double lat = 0, lon = 0, altKm = 0, posTime = 0;
    std::vector<float> track;    // lat, lon pairs of the recent ephemeris positions
    bool hasElements = false;
    float inclDeg = 0, raanDeg = 0, argLatDeg = 0, periodMin = 0;
    std::vector<double> downlinksMhz, uplinksMhz;
};

struct OrbcommChannel {
    bool active = false;
    int state = 0;               // 0 free, 1 acquiring (carrier from the 4th power), 2 tracking, 3 frame lock
    double offsetHz = 0;         // carrier against the radio centre
    double freqHz = 0;           // absolute (or the offset), as OrbcommSat::freqHz
    double dopplerHz = 0;
    float snrDb = 0;
    float levelDb = -120;
    int sat = -1;
    double clockPpm = 0;
    uint64_t frames = 0, pktOk = 0, pktBad = 0, syncLost = 0;
    std::vector<cf32> constel;   // recent differential products (unit), at +/-90 degrees when clean
    std::vector<float> eye;      // traces of the phase step over two symbols (in quarter turns), eyeLen points each
    int eyeLen = 0;
};

struct OrbcommTelemetry {
    // ---- the part every mode has
    uint64_t seq = 0;
    int state = 0;               // 0 searching, 1 carrier found, 2 frames decoding
    double cfoHz = 0;            // Doppler of the first decoder
    float snrDb = 0;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // packets with a good / bad checksum

    // ---- Orbcomm
    double timeSec = 0, centerHz = 0, inputRate = 0;
    int maxChannels = kOrbcommChannels;
    OrbcommChannel ch[kOrbcommChannels];
    std::vector<OrbcommSat> sats;
    std::vector<OrbcommPacket> log;   // oldest first
    uint64_t typeCount[orbcomm::kTypeSlots] = {};
    uint64_t messages = 0;            // messages put together from their parts
    std::string note;
};

inline std::string orbcommSummary(const OrbcommTelemetry& t) {
    char b[240];
    int n = snprintf(b, sizeof b, "Orbcomm  packets ok %llu bad %llu  satellites %zu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.sats.size());
    for (int i = 0; i < kOrbcommChannels && n < (int)sizeof b; i++) {
        const OrbcommChannel& c = t.ch[i];
        if (!c.active) continue;
        n += snprintf(b + n, sizeof b - n, "  [%d] %.4f MHz sat %d dop %+.0f Hz %.1f dB%s", i, c.freqHz / 1e6, c.sat, c.dopplerHz, c.snrDb, c.state == 3 ? " lock" : "");
    }
    return b;
}

} // namespace dect2
