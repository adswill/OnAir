// Analog TV, FM video: the frequency modulation of analog FPV links (5.8 GHz and 1.2 GHz video transmitters) instead of the amplitude
// modulated vision carrier of broadcast TV. The composite video is the frequency of the carrier: discriminator, a low-pass at the video band
// edge (which also takes out the audio subcarriers above it) and a decimation to about 10 Msps, the rate the picture decoder works at.
// The decoder levels itself from the sync pulses, so only the polarity matters here: its input has the sync tip at the maximum.
#pragma once
#include "dect2/ring.h"
#include <vector>

namespace dect2 {

class AtvFmDemod {
public:
    void configure(double fs);               // the input rate; the output is fs / decimation()
    void reset();
    // true (the usual): the sync tip is the lowest frequency, white the highest. false: the other way round.
    void setSyncLow(bool low) { syncLow_ = low; }
    bool syncLow() const { return syncLow_; }
    double videoRate() const { return fv_; }
    int decimation() const { return decim_; }
    // up to any number of input samples (DC removed); returns how many video samples are in v()
    size_t process(const cf32* x, size_t n);
    // The video, sync tip = larger, scaled so that a 16 MHz peak-to-peak swing is about 1 (v), and the analytic signal the colour decoder mixes down
    // (i = v, q = its Hilbert transform: a real signal alone would leave the image of the mixer, at twice the subcarrier, folded into the band).
    // All three are delayed by the same number of samples.
    const float* v() const { return v_.data(); }
    const float* i() const { return i_.data(); }
    const float* q() const { return q_.data(); }

private:
    double fs_ = 0, fv_ = 0;
    int decim_ = 1;
    bool syncLow_ = true;
    cf32 prev_ = cf32(1, 0);
    size_t phase_ = 0;                       // input samples to skip before the next output
    std::vector<float> taps_;                // symmetric low-pass, padded with zeros to a multiple of 8
    int nTaps_ = 0;                          // the length of taps_ (a multiple of 8)
    std::vector<float> buf_, out_;           // discriminator output with the history in front of it; the low-passed video samples
    std::vector<float> hist_, v_, i_, q_;    // the video samples with the history the Hilbert filter reaches back to; the outputs
    std::vector<float> hil_;                 // the Hilbert filter, odd lags only: hil_[j] is the weight at lag 2 j + 1
};

} // namespace dect2
