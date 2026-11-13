// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Dynamic noise floor for the fast scanner: one adaptive floor value per bin of the stitched spectrum, so a detection threshold is
// "N dB above the local floor" and follows the tilt of the band, the tuner's hop-edge roll-off and slow changes of the noise.
//
// DynamicFloor: a stochastic low-percentile tracker per bin. A bin's floor moves down fast when a sample is below it (release) and up slowly
// when it is above it (attack); the two rates set which percentile of the bin's noise it settles on (attack/(attack+release)). A sample far
// above the floor (a signal) does not raise it at all (outlier rejection), so a carrier that stays does not eat its own threshold. When most
// bins of a hop jump together (a gain change, a different antenna) the whole hop is shifted at once, since a majority of bins is not a signal.
//
// HopCalibration: the shape every hop has in common (roll-off at the edges, the DC spike in the middle), measured as a running median of
// (bin - the hop's own level) per position in the hop and subtracted before the floor and the detector see the spectrum.
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace fastscan {

struct FloorParams {
    double attackDbPerSec = 0.1;     // rise rate of the floor towards a sample above it (inside the outlier limit)
    double releaseDbPerSec = 0.3;    // fall rate towards a sample below it: attack/(attack+release) is the percentile the floor settles on (25 %)
    double fastFallDb = 3.0;         // a sample this far below the floor means the floor is too high: it moves half the way at once
    double outlierDb = 6.0;          // a sample this far above the floor never raises it
    double maxStepDb = 3.0;          // largest rise per update (a single bad sample cannot throw the floor)
    double jumpDb = 3.0;             // the median of a hop moved this far from its floors (up or down): the whole hop is shifted
    double maxDtSec = 5.0;           // the time since a bin's last update counts at most this much
};

class DynamicFloor {
public:
    void configure(size_t bins, const FloorParams& p);
    size_t bins() const { return floor_.size(); }
    // x: dB of bins [first, first+n); nowSec: time of this observation. Updates the floors of those bins. A bin seen for the first time starts at
    // min(x - 1, seedMaxDb): pass the hop's median so a signal present from the first look does not become its own floor.
    void update(size_t first, const float* x, size_t n, double nowSec, float seedMaxDb = 1e9f);
    float floorDb(size_t i) const { return floor_[i]; }
    const std::vector<float>& floors() const { return floor_; }
    bool seeded(size_t i) const { return seen_[i] != 0; }
    const FloorParams& params() const { return p_; }
    // persistence: per band file (header: magic, start, step, count; then floors in 0.01 dB). load() refuses a file of another band.
    bool save(const std::string& path, double startHz, double stepHz) const;
    bool load(const std::string& path, double startHz, double stepHz);
private:
    FloorParams p_;
    std::vector<float> floor_;
    std::vector<double> last_;
    std::vector<uint8_t> seen_;
    std::vector<float> tmp_;
};

class HopCalibration {
public:
    void configure(size_t binsPerHop);
    // in: one hop's bins (binsPerHop of them). Updates the profile and writes in - profile to out.
    void apply(const float* in, float* out);
    const std::vector<float>& profile() const { return prof_; }
    int hopsSeen() const { return n_; }
    void reset() { std::fill(prof_.begin(), prof_.end(), 0.f); n_ = 0; }
private:
    std::vector<float> prof_, tmp_;
    int n_ = 0;
};

}  // namespace fastscan
}  // namespace dect2
