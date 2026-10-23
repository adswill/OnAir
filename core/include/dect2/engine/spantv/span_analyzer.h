// Whole-span TV: occupied-signal detection on a wide capture (docs/WHOLESPAN-TV-DESIGN.md 1.1). An averaged power spectrum, a noise floor that
// is a curve (a line fitted through the quiet parts, so the tilt of an LNB's IF or the radio's roll-off does not hide or invent signals), then
// every run of bins above floor + threshold (2 dB hysteresis) becomes a SignalCand: centre, -3 dB width (the symbol rate of a root raised cosine
// carrier), occupied width, shoulder width and the roll-off it gives. Runs narrower than the narrowest signal of the standard (tones, the LNB
// beacon, spurs) are dropped.
#pragma once
#include "adapter.h"
#include <memory>
#include <vector>

namespace dect2 {

class Fft;

class SpanAnalyzer {
public:
    // bins: FFT size (a power of two, 1024..65536); takeEvery: one block in this many goes through the FFT
    void configure(double spanRate, int bins = 4096, int takeEvery = 4);
    void reset();
    void feed(const cf32* x, size_t n);
    size_t frames() const { return nFrames_; }
    double binHz() const { return bins_ > 0 ? rate_ / bins_ : 0; }
    int bins() const { return bins_; }

    struct Params {
        double thresholdDb = 6;     // a run starts this far above the floor (and ends 2 dB lower)
        double minBwHz = 50e3;      // narrower runs are not signals
        double maxBwHz = 1e12;
        double edgeFrac = 0.46;     // the usable part of the span: +- this fraction of the rate
    };
    std::vector<SignalCand> detect(const Params& p) const;
    // the spectrum (dB, DC in the middle) and the floor curve of the last detect() (tests, the spectrum overlay)
    std::vector<float> spectrumDb() const;
    std::vector<float> floorCurveDb() const { return floor_; }

    // the nearest of the standard roll-offs 0.35 0.25 0.20 0.15 0.10 0.05 (0 for a negative or absurd value)
    static double snapRollOff(double a);

private:
    double rate_ = 0;
    int bins_ = 4096, take_ = 4;
    std::vector<float> acc_, win_;
    std::vector<cf32> blk_, buf_;
    size_t fill_ = 0, nFrames_ = 0, blocks_ = 0;
    mutable std::vector<float> floor_;
    std::shared_ptr<Fft> fft_;
};

} // namespace dect2
