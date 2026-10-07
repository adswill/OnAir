// ADS-B pulse demodulator: complex baseband at any rate of 2 Msps or more -> sliced Mode S frames.
//
// The signal is on-off keyed, so only the power of the samples matters. Power is integrated over fractional windows (a running sum with linear
// interpolation at the edges), which makes the detector independent of the sample rate: 2.4 or 10 samples per microsecond work the same way.
//   0. below 4.5 Msps the complex samples are first interpolated to twice the rate (a 12 tap half-sample filter): a pulse that falls between two samples
//      of a 2 Msps stream would otherwise be seen with half its power in each of them
//   1. power per sample, with the DC offset of the radio removed
//   2. power per 0.25 microsecond bin (the working grid of the search), and the noise floor from the quiet blocks
//   3. a score for the preamble (pulses at 0, 1, 3.5 and 4.5 us, quiet in between and for the 3 us before the data) at every bin
//   4. for each score peak: timing refined in steps of 0.05 us, then 112 bits sliced by comparing the energy of the first and the second
//      half of every microsecond, handed to the checks (CRC, address); a frame that passes is skipped over
#pragma once
#include "adsb_track.h"
#include "ring.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace dect2 {

class AdsbDemod {
public:
    // Called for every candidate that passes the preamble test. Returns true when the frame was good (the demodulator then skips its length).
    // `last` is false when the demodulator may try the same frame again with other settings: a failure is then not counted yet.
    using Sink = std::function<bool(AdsbRaw&, bool last)>;

    void configure(double rate);
    void reset();                                  // forget the signal and the noise estimate; the clock keeps running
    void setSink(Sink s) { sink_ = std::move(s); }
    void feed(const cf32* x, size_t n);

    bool ready() const { return inRate_ >= 2e6 - 1; }
    double timeSec() const { return rate_ > 0 ? (double)samples_ / rate_ - delaySec_ : 0; }
    double noiseDbfs() const { return n0_ > 0 ? 10.0 * std::log10((double)n0_) : -120.0; }
    bool noiseReady() const { return noiseReady_; }
    uint64_t candidates() const { return candidates_; }     // preamble tests passed on the coarse grid
    uint64_t preambles() const { return preambles_; }       // ... and again after the timing was refined: frames that were sliced
    // test knobs
    void setThresholds(float pulseOverNoise, float pulseOverGap) { kPulse_ = pulseOverNoise; kGap_ = pulseOverGap; }

private:
    void pushSamples(const cf32* x, size_t m);
    void makeBins();
    void scanCandidates();
    bool tryFrame(int64_t bin);
    void compact();
    void updateNoise(float bin);
    float pulseThreshold() const { return n0_ * std::max(kPulse_, 1.0f + 2.4f * cv_); }   // what a pulse window must exceed

    double inRate_ = 0;                             // the rate of the input
    double rate_ = 0, spb_ = 0, spus_ = 0;          // the working rate (the input's, or twice that), samples per bin and per microsecond
    bool up_ = false;                               // interpolating to twice the rate
    double delaySec_ = 0;                           // delay of the interpolation filter
    cf32 hist_[11];                                 // the last input samples before the next chunk
    float taps_[12] = {};
    std::vector<cf32> tmp_, z_;
    Sink sink_;
    float kPulse_ = 3.0f, kGap_ = 2.0f;

    uint64_t samples_ = 0;                          // samples fed since the start (the clock)
    uint64_t smpBase_ = 0;                          // global index of pw_[0]
    std::vector<float> pw_;                         // power per sample
    std::vector<double> cum_;                       // cum_[i] = sum of pw_[0 .. i-1]
    float dcRe_ = 0, dcIm_ = 0; bool dcInit_ = false;

    int64_t binBase_ = 0;                           // global index of bin_[0]
    std::vector<float> bin_;                        // mean power per 0.25 us bin
    std::vector<float> score_;                      // preamble score per bin, same indexing
    int64_t scoreEnd_ = 0;                          // bins below this have a score
    int64_t scanBin_ = 0, skipUntil_ = 0;

    // noise floor: the lower quartile of the mean power of the last 16 blocks of 256 bins, and how much the power of a 0.5 us window varies in it
    // (cv, standard deviation over mean: about 1 for Gaussian noise, more when the samples are rounded to so few levels that noise is a handful of
    // single steps; the pulse threshold then goes up with it)
    float n0_ = 0, cv_ = 1; bool noiseReady_ = false;
    double blockSum_ = 0, blockSumW_ = 0, blockSumW2_ = 0; int blockCount_ = 0; float prevBin_ = 0;
    float blocks_[16] = {}, blockCv_[16] = {}; int blockN_ = 0, blockPos_ = 0;

    uint64_t candidates_ = 0, preambles_ = 0;
};

} // namespace dect2
