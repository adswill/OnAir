// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// EDACS test signal of a whole system as one wide capture: the control channel (the frames of edacs_proto.h carrying the messages of edacs_msg.h) and analog FM voice channels with a tone, at the offsets their LCNs have in the frequency table, plus a carrier offset and noise.
// The receiver is tested against this, not against a real system.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/edacs/edacs_msg.h"
#include <cstdint>
#include <vector>

namespace dect2 {

struct EdacsGenCall {
    int lcn = 3;
    int gid = 777, lid = 4321;
    bool group = true, emergency = false;
    double startSec = 1.0, endSec = 3.0;     // the carrier on the voice channel
    double announceLeadSec = 0.05;           // the assignment goes out this long before the carrier
    bool announce = true;                     // false: a carrier only
    bool sendEnd = true;                     // a call end message when the carrier stops
    bool digital = false;                    // ProVoice: 9600 bit/s frames of generated IMBE data (the seed below) instead of a tone
    uint32_t pvSeed = 1;
    double toneHz = 1000, devHz = 3000;      // the voice: a tone
    float amp = 0.4f;
};

struct EdacsGenConfig {
    double rate = 480000;
    double centreHz = 856100000;             // where the radio is tuned
    double controlHz = 856000000;            // the control channel
    int controlLcn = 1;
    int siteId = 5, systemId = 1234, nChannels = 8;
    double durSec = 8;
    double noise = 0.01;                     // per component, relative to a control channel amplitude of 0.5
    double cfoHz = 0;                        // the whole capture is off by this much
    float controlAmp = 0.5f;
    float controlDevHz = 1400;
    uint32_t seed = 1;
    edacs::Layout layout;
    std::vector<EdacsGenCall> calls;
    std::vector<double> lcnFreqHz;           // the table: LCN 1 first (needed to place the voice channels)
};

std::vector<cf32> edacsSystemSignal(const EdacsGenConfig& c);

// The ProVoice information bits of frame number n of a generated call (88 bits, 7100 arrangement), also for the tests
void edacsPvInfo(uint32_t seed, int frame, uint8_t info[88]);

} // namespace dect2
