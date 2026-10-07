// A fast streaming resampler for the ATSC 3.0 test signal (internal to atsc3_synth*.cpp): the signal fills 5.2 MHz of the 6.144 MHz it is made at, so a
// 24 tap windowed sinc with 2048 phases is enough (images below -60 dB) and about three times cheaper than the general ExactResampler. For any
// ratio (the position is tracked in double precision, so a clock offset of a few ppm is exact), output rates from 0.9 times the input rate up.
#pragma once
#include "dect2/ring.h"
#include <vector>

namespace dect2 {
namespace atsc3synth {

class FastResampler {
public:
    bool configure(double inRate, double outRate);
    // Appends the resampled samples to `out`.
    void process(const cf32* in, size_t n, std::vector<cf32>& out);

private:
    static constexpr int kTaps = 24, kPhases = 2048;
    bool pass_ = true;
    double step_ = 1.0;                // input samples per output sample
    std::vector<float> bank_;          // (kPhases + 1) x kTaps
    std::vector<float> re_, im_;       // input with history
    double pos_ = 0;                   // position of the next output in re_ (index of the sample it sits on, fractional part for the phase)
};

} // namespace atsc3synth
} // namespace dect2
