// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Burst detector for one watched frequency: a level that rises at least riseDb above the running median of the quiet samples,
// for at least minMs, counts as a burst. Samples are dB values with a time stamp (ms); no radio, no threads.
#pragma once
#include <cstddef>
#include <vector>

namespace dect2 {

struct BurstConfig {
    double riseDb = 6;          // rise above the running median that starts a burst
    double minMs = 200;         // the rise must last at least this long (first to last sample above it)
    size_t medianWindow = 15;   // quiet samples in the running median
};

struct BurstEvent {
    double startMs = 0;         // time of the first sample above the threshold
    double durationMs = 0;      // first to last sample above the threshold
    float peakDb = 0;           // highest sample in the burst
    float baseDb = 0;           // running median when the burst started (held during the burst)
    size_t samples = 0;         // samples above the threshold
};

// Streaming form: feed samples in time order; a burst is reported when it ends.
class BurstDetector {
public:
    explicit BurstDetector(const BurstConfig& cfg = BurstConfig{});
    // Returns true and fills ev when a burst that lasted long enough has just ended.
    bool push(float db, double tMs, BurstEvent& ev);
    // Ends a burst still open at the end of the input.
    bool flush(BurstEvent& ev);
    void reset();
    bool inBurst() const { return inRun_; }
    float lastBaseDb() const { return lastBase_; }

private:
    bool finish(BurstEvent& ev);
    void addQuiet(float db);
    float median() const;

    BurstConfig cfg_;
    std::vector<float> quiet_;  // ring of the last quiet samples
    size_t next_ = 0;
    bool inRun_ = false;
    double startT_ = 0, lastT_ = 0, lastBase_ = 0;
    float base_ = 0, peak_ = 0;
    size_t n_ = 0;
};

// Whole series: db[i] is taken at time i * dtMs.
std::vector<BurstEvent> detectBursts(const std::vector<float>& db, double dtMs, const BurstConfig& cfg = BurstConfig{});

} // namespace dect2
