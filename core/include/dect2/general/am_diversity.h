// Skywave help for AM. FadeTracker predicts the next fade of a carrier from its own history; AmDiversity plays the better of two frequencies that carry the same programme
// (the user pairs them, from one wide capture), with the sound aligned by cross-correlation and a short crossfade at each change.
#pragma once
#include "dect2/dsp/fftutil.h"
#include <cstdint>
#include <deque>
#include <atomic>
#include <memory>
#include <mutex>
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

class AudioOut;

// The two receivers' sound (their taps come from their own threads) through AmDiversity to a sound device of its own.
class AmDivPlayer {
public:
    AmDivPlayer();
    ~AmDivPlayer();
    void start(int rate = 48000, bool silent = false);   // silent: tests, no sound device
    void stop();
    void pushA(const float* x, size_t n);
    void pushB(const float* x, size_t n);
    void setSnr(float a, float b) { snrA_ = a; snrB_ = b; }
    void setVolume(float v);
    int active() const;
    int switches() const;
    int delaySamples() const;
    size_t outputs() const { return outputs_; }
private:
    void pump();
    AmDiversity div_;
    std::unique_ptr<AudioOut> audio_;
    std::mutex mu_;
    std::vector<float> a_, b_, out_;
    std::atomic<float> snrA_{0}, snrB_{0};
    float vol_ = 1;
    bool running_ = false;
    std::atomic<size_t> outputs_{0};
};

} // namespace dect2
