// DTMB receiver front end: the square-root raised cosine matched filter and the conversion from the radio's sample rate to the symbol rate of
// 7.56 Msym/s, in one polyphase filter whose sampling instants can be moved and whose spacing can be trimmed (timing recovery).
#pragma once
#include "ring.h"
#include <cmath>
#include <cstdint>
#include <vector>

namespace dect2::dtmb {

class SrrcResampler {
public:
    // False when the input rate is too low (the signal is 7.94 MHz wide: 8 Msps and up)
    bool configure(double inRate);
    void reset();
    // Input and output are decoupled so that timing corrections act on the very next sample: push() stores radio samples (with the DC
    // offset removed), pull() computes up to maxOut symbol-rate samples from what is stored and appends them to out.
    void push(const cf32* in, size_t n);
    size_t pull(std::vector<cf32>& out, size_t maxOut);
    // Everything at once
    void process(const cf32* in, size_t n, std::vector<cf32>& out) { push(in, n); pull(out, (size_t)-1); }
    // Symbol-rate samples that pull() could deliver now
    size_t available() const;
    // Moves the following sampling instants by `symbols` symbol periods (positive: later)
    void shift(double symbols) { pos_ += (int64_t)std::llround(symbols * ratio_ * kOne); }
    // Output spacing = one symbol * (1 + s)
    void setSpacing(double s) { step_ = (int64_t)std::llround(ratio_ * (1.0 + s) * kOne); spacing_ = s; }
    double spacing() const { return spacing_; }
    double ratio() const { return ratio_; }       // input samples per symbol
    int taps() const { return taps_; }
    void setDcRemoval(bool on) { dcOn_ = on; }    // on by default

private:
    static constexpr double kOne = 4294967296.0;  // positions are 32.32 fixed point, in input samples
    double inRate_ = 0, ratio_ = 1, spacing_ = 0;
    int64_t pos_ = 0, step_ = 0;
    int half_ = 0, taps_ = 0, stride_ = 0, phases_ = 512;
    std::vector<float> bank_;
    std::vector<float> re_, im_;    // buffered input
    size_t fill_ = 0;               // valid samples in re_ / im_
    double dc_re_ = 0, dc_im_ = 0;  // slow DC estimate removed from the input
    long dcCount_ = 0;
    bool dcOn_ = true;
};

} // namespace dect2::dtmb
