// Skywave help for AM. FadeTracker predicts the next fade of a carrier from its own history; AmDiversity plays the better of two frequencies that carry the same programme
// (the user pairs them, from one wide capture), with the sound aligned by cross-correlation and a short crossfade at each change.
#pragma once
#include "dect2/dsp/fftutil.h"
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace dect2 {

// The carrier level over time (dB). The fade period comes from the autocorrelation of the level; nextFadeIn() says when the next dip is due.
class FadeTracker {
public:
    void configure(double updatesPerSec);
    void reset();
    void push(float carrierDb);
    float depthDb() const { return depth_; }        // 10th-90th percentile spread
    float periodSec() const { return period_; }     // 0: no regular fade found
    float nextFadeIn() const { return next_; }      // seconds to the next dip, -1 unknown
    bool bestMoment() const;                        // the carrier is near its best right now (a good instant to record or to trust the audio)
private:
    double rate_ = 4;
    std::deque<float> h_;
    float depth_ = 0, period_ = 0, next_ = -1;
};

class AmDiversity {
public:
    AmDiversity();
    ~AmDiversity();
    void configure(int rate = 48000);
    void reset();
    // A block of each channel (the same length), with their current measures (audio SNR, dB). The output (mono, n samples, delayed by about 1 s) is appended to out.
    void process(const float* a, const float* b, size_t n, float snrA, float snrB, std::vector<float>& out);
    int active() const;                 // 0 or 1
    int delaySamples() const;           // channel 1 lags channel 0 by this many samples (negative: leads)
    int switches() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
