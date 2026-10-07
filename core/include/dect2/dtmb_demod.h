// DTMB demodulator pieces on symbol-rate samples: channel estimate from a PN header, removal of the header's leakage into the frame body
// (TDS-OFDM), the 3780 point FFT and the equaliser, and the system information decoder.
#pragma once
#include "dtmb_defs.h"
#include "dtmb_fft.h"
#include "ring.h"
#include <vector>

namespace dect2::dtmb {

// A channel impulse response: taps at lags m = -pre .. post-1 symbols from the nominal position of the header
struct Taps {
    int pre = 0, post = 0;
    std::vector<cf32> v;                      // post + pre values, v[m + pre]
    float noise = 0;                          // noise power per received sample, from the fit residual
    float tapVar = 0;                         // variance of the estimate of one tap (noise only)
    float peakPower = 0;                      // power of the strongest tap
    int peakIndex = 0;                        // its lag
    float peakFrac = 0;                       // fractional lag of the peak (parabolic interpolation, -0.5 .. 0.5)
    float energy = 0;                         // sum of the tap powers
    cf32 at(int m) const { return (m >= -pre && m < post) ? v[(size_t)(m + pre)] : cf32(0, 0); }
};

// Least squares estimate of the channel from the PN header (known chips, header amplitude from the power ratio)
class HeaderEstimator {
public:
    explicit HeaderEstimator(Header h);
    void setWindow(int pre, int post);
    int pre() const { return pre_; }
    int post() const { return post_; }
    // r: the header's first sample; the header (length symbols) must be fully inside what r points at. phase: PN phase of the frame.
    void estimate(const cf32* r, int phase, Taps& out);
    // Zero the taps that do not stand out of the estimation noise and fill in the peak data
    static void clean(Taps& t, float sigmaTap, float threshold);

private:
    Header h_;
    int pre_ = 0, post_ = 0, t0_ = 0, rows_ = 0;
    double amp_;                              // amplitude of one header symbol component (symbol = amp (1 + j) chip)
    std::vector<double> minv_;                // PN595: inverse of the normal matrix, L x L
    std::vector<float> chipf_, re_, im_;
};

// Removes the header leakage, folds the body and takes the FFT; gives the equalised carriers
class BodyEqualizer {
public:
    BodyEqualizer();
    // r: first symbol of the BODY of the frame; valid for index -pre .. kBody + post - 1. ha / hb: cleaned taps of the header before / after,
    // chipsA / chipsB: their chips (+-1, header length) with the header amplitude `amp`. The frequency response used is the mean of the two.
    // bins: the 3780 equalised carriers in physical order (FFT bin order), var: noise variance of each (E|n|^2), both unit power constellation scale
    // up to the unknown gain of the header relative to the body.
    void run(const cf32* r, const Taps& ha, const int8_t* chipsA, const Taps& hb, const int8_t* chipsB, int headerLength, double amp, cf32* bins, float* var);
    // frequency response of the last run (3780 values), for the display
    const std::vector<cf32>& response() const { return resp_; }

private:
    MixedFft fft_;
    std::vector<cf32> z_, y_, hh_, resp_;
};

// Body of a single carrier frame (C=1): the 3780 symbols in transmit order, after the leakage of the headers is removed and the channel is undone by a
// frequency domain minimum mean square error filter (4096 points). The first 36 symbols are the system information. var: error variance of every symbol.
class SingleCarrierEqualizer {
public:
    static constexpr int kFft = 4096;
    SingleCarrierEqualizer();
    // r, ha, chipsA, hb, chipsB, Lh, amp: as for BodyEqualizer::run (pre + post must stay below 4096 - 3780)
    void run(const cf32* r, const Taps& ha, const int8_t* chipsA, const Taps& hb, const int8_t* chipsB, int Lh, double amp, cf32* symbols, float* var);

private:
    MixedFft fft_;
    std::vector<cf32> z_, y_, hh_;
};

// System information: correlation of the 36 equalised system information symbols with the 22 defined words (index 3 .. 24).
// w: weight of every symbol (1 / noise variance). scores[i] in -1 .. 1 for index i + 3.
void siScores(const cf32* si36, const float* w36, float* scores22);

} // namespace dect2::dtmb
