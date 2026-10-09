// Sample-clock error correction for OFDM receivers (DVB-T2 uses it; meant for DVB-T, ISDB-T, DTMB, ATSC 3.0 and DAB too).
#pragma once
#include "ring.h"
#include <vector>

namespace dect2 {

// Clock-error corrector for a stream that is already at the right nominal rate: every output advances `step` (close to 1) input samples, so
// a sample clock that runs fast by x ppm is undone with step 1 + x * 1e-6. Changing the step changes the slope only; the position stays
// continuous, so a tracking loop can steer it without jumps. Windowed sinc of kTaps taps, kPhases phases (nearest), flat to 0.43 fs.
class ClockCorrector {
public:
    static constexpr int kTaps = 24, kPhases = 512;
    ClockCorrector();
    void setStep(double s) { step_ = s; }
    void setPpm(double ppm) { step_ = 1.0 + ppm * 1e-6; }   // the clock runs ppm fast (positive) or slow
    double step() const { return step_; }
    // Restarts with `n` samples of history: the next output lands exactly on the next input sample (no shift in time when it is switched
    // in mid-stream). Fewer than kTaps / 2 samples are padded with silence.
    void prime(const cf32* hist, size_t n);
    void process(const cf32* in, size_t n, std::vector<cf32>& out);

private:
    std::vector<float> bank_;      // (kPhases + 1) x kTaps
    std::vector<float> re_, im_;   // input with history
    double pos_ = 0;               // next output, in input samples from re_[0]
    double step_ = 1.0;
};

} // namespace dect2
