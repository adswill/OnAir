// DTMB frame acquisition: finds the PN headers in a block of symbol-rate samples (FFT correlation with the three header sequences), the header
// mode and the position of a frame inside the super-frame (from the way the PN phase moves from frame to frame).
#pragma once
#include "dtmb_defs.h"
#include "fftutil.h"
#include "ring.h"
#include <memory>
#include <vector>

namespace dect2::dtmb {

struct AcqResult {
    bool ok = false;
    Header header = Header::Pn945;
    bool rotates = true;       // the PN phase changes from frame to frame (PN420, PN945 with phase rotation)
    long start = 0;            // index in the block of the first symbol of the header of the last frame of the chain
    int frame = 0;             // its number inside the super-frame (0 when the phase does not rotate)
    int hits = 0;              // frames in the chain
    float metric = 0;          // mean normalised correlation of the chain
    double cfoHz = 0;          // carrier offset estimated from the headers (relative to the block as given; within +-14 kHz of it)
    float coherence = 0;       // share of the header energy that adds up at that offset (about C/N / (1 + C/N))
    std::vector<long> starts;  // header starts of the whole chain
};

class Acquirer {
public:
    static constexpr int kBlock = 65536;
    static constexpr float kMinCoherence = 0.04f;
    double symRate = kSymbolRate;   // of the channel width (symbolRateFor)
    Acquirer();
    // r: kBlock symbol-rate samples. `threshold`: normalised correlation a header peak must reach (a carrier wave 10 dB above the signal cuts it to 0.3; noise alone reaches 0.2
    // somewhere in a block for the 255 chip core, which is why a hypothesis needs its peak in at least five frames in a row).
    bool run(const cf32* r, AcqResult& out, float threshold = 0.20f);
    // For tests and the display: the largest normalised correlation seen by the last run for each header, and noise floor
    float lastPeak(Header h) const { return peak_[(int)h]; }

private:
    Fft fft_;
    std::vector<cf32> spec_[3];       // conjugated spectra of the references
    std::vector<cf32> work_, x_;
    std::vector<float> rho_, rmax_, energy_;
    float peak_[3] = {0, 0, 0};
    void correlate(int ref, const cf32* xs, int len);
    double refineCfo(const cf32* r, const AcqResult& res, float& coherence) const;
};

} // namespace dect2::dtmb
