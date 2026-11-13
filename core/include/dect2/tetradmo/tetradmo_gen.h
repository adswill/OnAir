// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA Direct Mode test signal: one call (DSBs with DM-SETUP, then traffic and DM-OCCUPIED, then DM-TX CEASED). No real radio.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/tetradmo/tetradmo_proto.h"
#include <cstdint>
#include <vector>

namespace dect2 {

struct TetraDmoGenConfig {
    double rate = 72000;
    double noise = 0.0;              // per component, against a burst amplitude of 0.5
    double cfoHz = 0;
    double sroPpm = 0;
    uint32_t seed = 3;
    uint32_t src = 0x1A2B3C, dest = 0x4D5E6F, mni = (262u << 14) | 1001u;
    bool groupCall = true, e2eEncrypted = false;
    int aiEncState = 0;
    int commType = 0;
    int trafficFrames = 40;
    int setupRepeats = 4;
    bool sendRelease = true;
};

// bits[k] of the burst number k (1 based in the DM frame) are the 432 type-5 traffic bits the generator sent
struct TetraDmoGenOut {
    std::vector<cf32> iq;
    std::vector<tetra::Bits> trafficBits;   // the type-4 bits (descrambled, still interleaved) of every traffic burst
};

TetraDmoGenOut tetraDmoSignal(const TetraDmoGenConfig& c);

} // namespace dect2
