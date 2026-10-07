// Front end of one GNSS signal band: mixes a signal's centre frequency to zero and resamples to the rate its channels work at (a multiple of
// the chip rate, chosen so that one code period is a power of two samples for the acquisition FFT). Any input rate from 2 Msps up, non-integer ratios included.
#pragma once
#include "ring.h"
#include <cstdint>
#include <vector>

namespace dect2 {

class GnssBand {
public:
    // fsIn: input rate; offsetHz: where the signal's carrier sits in the input (signal frequency minus tuned centre); fsOut: output rate;
    // keepSamples: how many output samples the buffer holds at least
    void init(double fsIn, double offsetHz, double fsOut, size_t keepSamples);
    void reset();
    // n input samples (already DC-corrected and scaled)
    void process(const cf32* x, size_t n);
    // output samples are numbered from 0 since the start; buf holds [base, base + buf.size())
    int64_t base() const { return base_; }
    int64_t end() const { return base_ + (int64_t)buf_.size(); }
    const cf32* at(int64_t idx) const { return &buf_[(size_t)(idx - base_)]; }
    // drop samples before idx (but never more than allowed by the keep length)
    void trimTo(int64_t idx);
    double rateOut() const { return fsOut_; }
    double offsetHz() const { return offset_; }
    double timeOf(double idx) const { return idx / fsOut_; }     // seconds since the start of the signal
    bool valid() const { return fsOut_ > 0; }
private:
    double fsIn_ = 0, fsOut_ = 0, offset_ = 0, ratio_ = 1;
    int taps_ = 16;
    static constexpr int kPhases = 256;
    std::vector<float> h_;           // kPhases x taps_
    std::vector<cf32> hist_;         // mixed input: the tail kept between calls plus the new block
    std::vector<cf32> mix_;          // table of the mixer rotation for one block
    int64_t inBase_ = 0;             // input index of hist_[0]
    int64_t nextOut_ = 0;            // next output sample to produce
    int64_t inTotal_ = 0;            // input samples consumed
    double mixPhase_ = 0;            // cycles
    // narrow-band interference excision: windowed overlap-add FFT frames of 2048 samples, bins far above the median power are zeroed
    void excise();
    static constexpr int kExN = 2048, kExHop = 1024;
    bool exEnable_ = true;
    std::vector<cf32> raw_;          // resampler output waiting for the excision stage
    int64_t rawBase_ = 0;            // output index of raw_[0]
    int64_t exNextFrame_ = 0;        // start index of the next frame
    std::vector<cf32> exTail_;       // the overlapping half of the previous frame, already windowed
    std::vector<float> exWin_;
    std::vector<float> exRe_, exIm_, exPow_, exA_, exB_;
    std::vector<char> exKill_;
public:
    uint64_t excisedBins = 0, frames = 0;
    void setExcision(bool on) { exEnable_ = on; }
private:
    std::vector<cf32> buf_;
    int64_t base_ = 0;
    size_t keep_ = 0;
};

} // namespace dect2
