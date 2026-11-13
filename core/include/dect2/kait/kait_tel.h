// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAIT receiver telemetry. RxTelemetry::kait, engine standard 133, active standard 132.
#pragma once
#include "dect2/kait/kait_codec.h"
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct KaitTelemetry {
    uint64_t seq = 0;
    bool dataValid = false;        // the last burst gave at least one good block (the engine's shared telemetry fields)
    uint64_t blocksOk = 0, blocksBad = 0;   // blocks of the last burst that passed / failed the CRC
    int state = 0;                 // 0 no input, 1 listening for a sweep, 2 burst in progress (capturing), 3 decoding, 4 last burst decoded (see headerOk)
    double captureSec = 0;         // state 2: seconds of the burst received so far
    bool headerOk = false;         // of the last burst
    std::string callsign;
    int width = 0, height = 0;
    int profile = 0;
    int nLayers = 0;
    int layersComplete = 0;        // leading layers with every block good
    int layerNow = -1;             // decoding: the layer just finished
    std::vector<int> layerBlocks, layerBlocksOk;   // per layer
    size_t bytesGood = 0, bytesTotal = 0;          // usable stream bytes, and what the burst carries
    float snrDb = 0;               // in the 190 kHz band
    double cfoHz = 0, clockPpm = 0;
    uint64_t bursts = 0, burstsDecoded = 0;        // sweeps that started a capture, bursts with a good header
    uint64_t imageSeq = 0;         // changes whenever `image` does
    std::shared_ptr<const kait::Image> image;      // the picture of the good bytes so far (null before the first layer)
    double inputRate = 0;
};

inline std::string kaitSummary(const KaitTelemetry& t) {
    char b[200];
    switch (t.state) {
    case 0: return "KAIT: waiting for samples";
    case 1: return "KAIT: listening for a picture burst";
    case 2: snprintf(b, sizeof b, "KAIT: receiving, %.1f of 10 s", t.captureSec); return b;
    case 3: return "KAIT: decoding";
    default: break;
    }
    if (!t.headerOk) return "KAIT: burst seen, header not decoded";
    snprintf(b, sizeof b, "KAIT: %s, %d of %d layers, %zu bytes, SNR %.0f dB", t.callsign.c_str(), t.layersComplete, t.nLayers, t.bytesGood, (double)t.snrDb);
    return b;
}

} // namespace dect2
