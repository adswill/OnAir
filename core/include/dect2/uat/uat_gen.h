// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#pragma once
#include "uat_frame.h"
#include "dect2/engine/mode_synth.h"
#include <memory>
#include <vector>

namespace dect2 {

struct UatTx {
    double timeSec = 0;
    uint8_t frame[UAT_LONG_FRAME_BYTES] = {};
    int frameBytes = UAT_SHORT_FRAME_DATA_BYTES;
    bool uplink = false;
    float gainDb = 0;
    double cfoHz = 0;
};

std::vector<cf32> uatBurst(const UatTx& tx, double rate);
std::vector<cf32> uatScene(double rate, double dur, const std::vector<UatTx>& txs, double snrDb, unsigned seed);

std::unique_ptr<ModeSynth> makeUatSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
