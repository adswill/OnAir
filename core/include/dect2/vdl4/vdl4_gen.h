// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDL Mode 4 test signal: GFSK bursts on VHF channels (synthetic STDMA traffic).
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct Vdl4GenOptions {
    int aircraft = 4;
    uint32_t seed = 1;
    double centerHz = 136.85e6;
    std::vector<double> channelsHz = {136.725e6, 136.775e6, 136.825e6, 136.875e6, 136.900e6, 136.975e6};
    double rateFactor = 1;
    double cfoSpreadHz = 120;
};

std::vector<uint8_t> vdl4SynthBurstBits(bool sync, uint32_t icao, int srcType, double lat, double lon, int altFt, const std::string& dlsText);
void vdl4RenderBits(const std::vector<uint8_t>& lineBits, cf32* out, int64_t outStart, size_t n, double rate, double start, double carHz, double amp, double phase);

std::unique_ptr<ModeSynth> makeVdl4Synth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
