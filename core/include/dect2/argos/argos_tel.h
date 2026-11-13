// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Argos receiver telemetry. RxTelemetry::argos, engine standard 154, active standard 153 (family 152). Receive only.
#pragma once
#include "dect2/argos/argos_proto.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

// One uplink burst (a PTT transmission) that was heard
struct ArgosBurst {
    int64_t wallTime = 0;
    double timeSec = 0;            // signal time of the burst's start
    int kind = 0;                  // 1 PTT-A2 decoded, 2 carrier and modulation but not A2 Manchester (A3 / ZE / Argos-4: not decoded)
    double carrierHz = 0;          // from the dial
    float snrDb = 0, levelDb = -200;
    float carrierMs = 0, durationMs = 0;
    float quality = 0;             // A2: how close the Manchester phase steps are to +-2.2 rad (1 = exact)
    argos::A2Message msg;          // kind 1
};

// One platform (28 bit ID) and the frequencies of its bursts: the Doppler and oscillator drift curve
struct ArgosPtt {
    std::string idHex;
    uint32_t id = 0;
    int count = 0;
    int64_t firstWall = 0, lastWall = 0;
    std::string lastData;          // hex
    std::vector<std::pair<double, double>> freq;   // (signal seconds, carrier Hz from the dial), oldest first, at most kArgosFreqMax
    double driftHzPerMin = 0;      // least-squares slope of freq (0 with fewer than 2 bursts)
};

struct ArgosDlFrame {
    int64_t wallTime = 0;
    double timeSec = 0;
    int coding = 0;                // 0 NRZ, 1 NRZ inverted, 2 NRZI
    bool fcsOk = false;
    std::string hex;
    int bytes = 0;
};

constexpr size_t kArgosLogMax = 300, kArgosPttMax = 200, kArgosFreqMax = 64, kArgosDopplerMax = 1200, kArgosDlMax = 300;

struct ArgosTelemetry {
    uint64_t seq = 0;
    int state = 0;                 // 0 no input, 1 listening, 2 something decoded in the last 5 minutes
    int link = 0;                  // the link in use: 0 uplink (401.65 MHz PTTs), 1 downlink (465.9875 MHz satellite)
    int linkSetting = 0;           // 0 automatic from the dial, 1 uplink, 2 downlink
    double dialHz = 0, inputRate = 0, timeSec = 0;
    double cfoHz = 0;              // the last burst's carrier (uplink) or the tracked carrier (downlink), Hz from the dial
    float snrDb = 0, levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // A2 messages decoded / bursts that were not / HDLC frames with a good FCS
    // uplink
    uint64_t bursts = 0, otherBursts = 0;
    std::vector<ArgosBurst> log;   // newest first
    std::vector<ArgosPtt> ptts;    // most recently heard first
    // downlink
    bool dlLocked = false;
    double dlCarrierHz = 0;        // Hz from the dial (Doppler plus the radio's offset)
    float dlCnrDb = 0;             // carrier line over the noise in the acquisition spectrum
    std::vector<std::pair<double, double>> doppler;   // (UNIX seconds, carrier Hz from the dial) while locked, oldest first
    uint64_t dlBits = 0, dlFlags[3] = {0, 0, 0}, dlFcsBad = 0;
    int dlCoding = -1;             // the coding that shows flags (0 NRZ, 1 inverted, 2 NRZI), -1 none yet
    std::vector<ArgosDlFrame> frames;   // newest first
};

inline std::string argosSummary(const ArgosTelemetry& t) {
    char b[200];
    if (t.state == 0) return "Argos: waiting for samples";
    if (t.link == 1) {
        if (!t.dlLocked) return "Argos downlink: searching for the carrier";
        snprintf(b, sizeof b, "Argos downlink: carrier %+.0f Hz, %llu frames", t.dlCarrierHz, (unsigned long long)t.blocksOk);
        return b;
    }
    if (t.log.empty()) { snprintf(b, sizeof b, "Argos: listening (%.0f dBFS)", t.levelDb); return b; }
    const ArgosBurst& e = t.log.front();
    if (e.kind == 1) snprintf(b, sizeof b, "Argos PTT %s, %d data bits", e.msg.idHex.c_str(), e.msg.dataBits);
    else snprintf(b, sizeof b, "Argos burst %+.0f Hz (not A2)", e.carrierHz);
    return b;
}

}  // namespace dect2
