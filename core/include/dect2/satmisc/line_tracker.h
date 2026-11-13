// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Narrow spectral lines followed through time: a short-time spectrum, a background estimated across frequency, peaks over it, and tracks that follow a
// moving line (Doppler). Shared by the satellite carrier modes and the GRAVES echo search.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/geo/doppler_fit.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace satmisc {

struct LineTrack {
    uint32_t id = 0;
    double startSec = 0, endSec = 0;   // seconds of input since configure()
    float peakSnrDb = 0;
    std::vector<DopplerSample> pts;    // t: seconds since configure(); hz: baseband frequency of the line
    double lastHz() const { return pts.empty() ? 0.0 : pts.back().hz; }
    double durationSec() const { return endSec - startSec; }
};

struct LineTrackerConfig {
    double rate = 12000;
    int fftLog2 = 12;
    int average = 1;                  // spectra averaged per detection frame
    double thresholdDb = 12;          // over the local background
    double minDurSec = 1.5;           // shorter tracks are dropped
    int minPoints = 6;
    double gapSec = 2.0;              // a track ends after this long unseen
    double maxSlopeHzS = 200;         // how fast a line may move
    double spanHz = 0;                // only |f| below this (0: everything but the band edges)
    double excludeHz = 0;             // ignore lines within this of excludeCentreHz (a beacon's own line)
    double excludeCentreHz = 0;
    size_t maxPoints = 4000;          // per track
    size_t maxActive = 64;
};

class LineTracker {
public:
    void configure(const LineTrackerConfig& c);
    void setExcludeCentre(double hz) { cfg_.excludeCentreHz = hz; }
    void push(const cf32* x, size_t n);
    std::vector<LineTrack> takeFinished();          // tracks that ended (long enough) since the last call
    const std::vector<LineTrack>& active() const { return active_; }
    double binHz() const { return binHz_; }
    double frameSec() const { return frameSec_; }
    double timeSec() const { return (double)samples_ / cfg_.rate; }
    const std::vector<float>& spectrumDb() const { return lastDb_; }   // DC centred, dB over the median background
private:
    void frame();
    LineTrackerConfig cfg_;
    int n_ = 0, hop_ = 0, fill_ = 0, avgN_ = 0;
    double binHz_ = 0, frameSec_ = 0;
    uint64_t samples_ = 0, frames_ = 0;
    uint32_t nextId_ = 1;
    std::vector<cf32> buf_;
    std::vector<float> win_, acc_, lastDb_;
    std::vector<LineTrack> active_, done_;
};

} // namespace satmisc
} // namespace dect2
