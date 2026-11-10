// Radiosonde receiver, one channel: mixer + decimating filters, FM discriminator, symbol timing, and the decoders of every sonde type.
// Private to the receiver (sonde_rx.cpp).
#pragma once
#include "dect2/common/ring.h"
#include "dect2/sonde/sonde_bits.h"
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {
namespace sondedsp {

// Windowed sinc low-pass (Kaiser), unity gain at DC; fc in cycles per sample
std::vector<float> kaiserLowpass(int taps, double fc, double beta);

// Complex in, real taps, decimation by R, with an optional mixer in front (multiplies by exp(-j 2 pi f n)).
class Decim {
public:
    void init(int R, int taps, double fcIn, double beta);
    void reset();
    void setMixer(double cyclesPerSample);       // phase continuous
    double mixer() const { return mixF_; }
    void process(const cf32* in, size_t n, std::vector<cf32>& out);
    int decimation() const { return R_; }
private:
    int R_ = 1, taps_ = 1;
    std::vector<float> h_;
    std::vector<float> re_, im_;
    size_t nextPos_ = 0;
    double mixF_ = 0, mixPh_ = 0;
    bool mixOn_ = false;
    std::vector<cf32> w_;                         // exp(-j 2 pi f k), k < kMixBlock
    static constexpr int kMixBlock = 256;
    void buildMixer();
};

// Hard symbol slicer with timing recovery on the (smoothed) discriminator output
class SymbolChain {
public:
    void init(double fs, double baud, bool afsk = false, bool midpoint = false);
    void reset();
    // one discriminator sample (Hz), the slicing threshold; appends symbols to syms
    inline void step(float d, float thr, std::vector<uint8_t>& syms);
    double baud() const { return baud_; }
    bool midpoint() const { return midpoint_; }
    int symbolsOut() const { return nOut_; }
    double symbolPhase() const { return ph_; }
private:
    double fs_ = 1, baud_ = 1, dp_ = 0, dp0_ = 0, ph_ = 0;
    int L_ = 1, wpos_ = 0;
    std::vector<float> win_;
    double wsum_ = 0;
    float prev_ = 0;
    bool havePrev_ = false;
    int nTrans_ = 0, nOut_ = 0;
    // AFSK (Bell 202): tones 1200 / 2200 Hz in the discriminator output; one tone power difference replaces the raw deviation
    bool afsk_ = false, midpoint_ = false;
    float max_ = 0, min_ = 0, decay_ = 0.001f;
    int afLen_ = 1, afPos_ = 0;
    std::vector<float> afBuf_;
    double afRotRe_[2] = {1, 1}, afRotIm_[2] = {0, 0}, afEndRe_[2] = {1, 1}, afEndIm_[2] = {0, 0};
    double afRe_[2] = {0, 0}, afIm_[2] = {0, 0};
    float afskStep(float x);
};

inline float SymbolChain::afskStep(float x) {
    const float old = afBuf_[(size_t)afPos_];
    afBuf_[(size_t)afPos_] = x;
    if (++afPos_ == afLen_) afPos_ = 0;
    // sliding DFT, a little loss keeps rounding from building up
    for (int t = 0; t < 2; t++) {
        const double re = afRe_[t] * afRotRe_[t] - afIm_[t] * afRotIm_[t], im = afRe_[t] * afRotIm_[t] + afIm_[t] * afRotRe_[t];
        afRe_[t] = 0.99999 * re + x - old * afEndRe_[t];
        afIm_[t] = 0.99999 * im - old * afEndIm_[t];
    }
    const double a = afRe_[0] * afRe_[0] + afIm_[0] * afIm_[0], b = afRe_[1] * afRe_[1] + afIm_[1] * afIm_[1];
    return (float)((a - b) / (a + b + 1e-6));   // 1200 Hz (tone 0) -> positive -> symbol 1
}

inline void SymbolChain::step(float d, float thr, std::vector<uint8_t>& syms) {
    if (afsk_) { d = afskStep(d - thr); thr = 0; }
    else if (midpoint_) {
        // a frame with a short preamble in an unbalanced line code: slice at the middle of the recent extremes, not at the mean
        if (d > max_) max_ = d; else max_ -= (max_ - d) * decay_;
        if (d < min_) min_ = d; else min_ += (d - min_) * decay_;
        thr = 0.5f * (max_ + min_);
    }
    wsum_ += d - win_[(size_t)wpos_];
    win_[(size_t)wpos_] = d;
    if (++wpos_ == L_) wpos_ = 0;
    const float s = (float)(wsum_ / L_) - thr;
    const double phPrev = ph_;
    ph_ += dp_;
    if (ph_ >= 1.0) ph_ -= 1.0;
    // zero crossing: the instant it happened inside the last sample interval, in symbol phase
    if (havePrev_ && ((prev_ < 0) != (s < 0))) {
        const double tc = prev_ / (prev_ - s);              // 0..1 of the interval
        double pc = phPrev + dp_ * tc;
        if (pc >= 1.0) pc -= 1.0;
        double e = pc > 0.5 ? pc - 1.0 : pc;                 // the edge should sit at phase 0
        const double g = 0.04 + 0.4 / (1.0 + 0.25 * nTrans_);
        ph_ -= g * e;
        dp_ -= 0.002 * g * e;
        const double lim = dp0_ * 0.02;                      // a clock cannot be more than 2 % off
        if (dp_ > dp0_ + lim) dp_ = dp0_ + lim;
        if (dp_ < dp0_ - lim) dp_ = dp0_ - lim;
        if (ph_ < 0) ph_ += 1.0;
        if (ph_ >= 1.0) ph_ -= 1.0;
        nTrans_++;
    }
    // the middle of the symbol
    if (phPrev < 0.5 && ph_ >= 0.5 && ph_ - phPrev < 0.5) {
        const double fr = dp_ > 0 ? (0.5 - phPrev) / (ph_ - phPrev) : 1.0;
        const float v = havePrev_ ? (float)(prev_ + fr * (s - prev_)) : s;
        syms.push_back(v > 0 ? 1 : 0);
        nOut_++;
    }
    prev_ = s;
    havePrev_ = true;
}

} // namespace sondedsp
} // namespace dect2
