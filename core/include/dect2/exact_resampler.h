// Streaming resampler for any ratio (windowed sinc, finely interpolated phases, position tracked in double precision), for receivers whose
// sample rate is not a small rational multiple of the radio's (a rational approximation is tens of ppm off at 10 Msps).
#pragma once
#include "ring.h"
#include <vector>

namespace dect2 {

class ExactResampler {
public:
    // Returns false for ratios the filter cannot do (the output rate must not exceed about 4 times the input rate).
    bool configure(double inRate, double outRate);
    bool passthrough() const { return pass_; }
    void reset();
    // Lets the output run slower (factor > 1: every output sample advances that much further in the input) or faster; for tracking a clock error.
    void scaleStep(double factor) { step_ *= factor; }
    double step() const { return step_; }
    // Appends resampled samples to `out`.
    void process(const cf32* in, size_t n, std::vector<cf32>& out);

private:
    bool pass_ = true;
    double step_ = 1.0;        // input samples per output sample
    int half_ = 16, taps_ = 32, stride_ = 32, phases_ = 2048;
    std::vector<float> bank_;  // (phases + 1) x stride
    std::vector<float> re_, im_;   // buffered input (with history)
    double pos_ = 0;           // position of the next output sample, in input samples counted from re_[0]
};

} // namespace dect2
