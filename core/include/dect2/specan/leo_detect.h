// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LEO downlink monitor, detect only: finds narrow carriers whose frequency drifts steadily over time (the Doppler of a satellite
// pass) in a stream of spectrum frames. Pure function: no radio, no threads; nothing is decoded.
#pragma once
#include <cstddef>
#include <vector>

namespace dect2 {

struct LeoBand {
    double startHz = 0;         // frequency of bin 0
    double binHz = 1;           // bin spacing
};

struct LeoFrame {
    double tMs = 0;             // frame time
    std::vector<float> db;      // one level per bin; bin i sits at startHz + i * binHz
};

struct LeoConfig {
    double riseDb = 8;          // a peak must rise this far above the frame median
    size_t maxWidthBins = 6;    // wider than this (above half the rise) is not a narrow carrier
    double maxDriftHzPerS = 400;// bound on the drift rate (LEO Doppler at VHF/UHF is a few hundred Hz/s)
    double maxGapMs = 1000;     // a track may miss frames for this long and still continue
    double minMs = 3000;        // a track must last at least this long
    size_t minHits = 5;         // and be seen in at least this many frames
    double minSwingHz = 300;    // and move at least this far (a fixed carrier is not a satellite)
};

struct LeoCarrier {
    double carrierHz = 0;       // first observed frequency
    double driftHzPerS = 0;     // least-squares slope of frequency over time
    double firstSeenMs = 0;     // time of the first frame with the carrier
    double lastSeenMs = 0;
    double durationMs = 0;
    float peakDb = 0;           // highest level seen
    size_t hits = 0;            // frames with the carrier
};

// Returns the carriers that pass every check in cfg, sorted by first-seen time.
std::vector<LeoCarrier> detectDopplerCarriers(const std::vector<LeoFrame>& frames, const LeoBand& band,
                                              const LeoConfig& cfg = LeoConfig{});

} // namespace dect2
