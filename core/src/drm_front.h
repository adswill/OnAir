// DRM receiver front end: any input rate to the 48 kHz complex baseband the OFDM receiver works on. Private to the DRM receiver.
#pragma once
#include "dect2/exact_resampler.h"
#include "dect2/ring.h"
#include <vector>

namespace dect2 { namespace drm {

// Low-pass FIR that keeps one output in D (complex in, real taps)
class DecimFir {
public:
    void design(double passHz, double stopHz, double inRate, int d, double attenDb = 70);
    void reset();
    void process(const cf32* in, size_t n, std::vector<cf32>& out);
    int decim() const { return d_; }
private:
    std::vector<float> h_;
    std::vector<cf32> hist_, x_;
    int d_ = 1, cnt_ = 0;
};

// Fractional resampler of the 48 kHz stream that takes out a sample clock offset: the output consumes (1 + ppm * 1e-6) input samples per output sample.
// Windowed sinc of 20 taps with 256 phases. The samples always go through the filter (a delay of 10 samples, constant).
class SroCorrector {
public:
    SroCorrector();
    void reset();
    void setPpm(double ppm) { ppm_ = ppm; }
    double ppm() const { return ppm_; }
    void process(const cf32* in, size_t n, std::vector<cf32>& out);
private:
    static constexpr int kTaps = 20, kPhases = 256;
    std::vector<float> bank_;       // (kPhases + 1) x kTaps
    std::vector<cf32> x_;           // history and new input
    double pos_ = 0, ppm_ = 0;
};

class DrmFront {
public:
    static constexpr double kOutRate = 48000.0;
    bool configure(double inRate);        // false: the rate is below 48 kHz
    bool ready() const { return ready_; }
    void reset();
    // Appends the 48 kHz samples of the n input samples (DC removed) to out
    void process(const cf32* in, size_t n, std::vector<cf32>& out);
    double levelDbfs() const;             // mean power of the 48 kHz stream since the last call, in dB re full scale
    void resetLevel() { pw_ = 0; pn_ = 0; }
private:
    double inRate_ = 0;
    bool ready_ = false, pass_ = false;
    std::vector<DecimFir> stages_;
    ExactResampler rs_;
    bool useRs_ = false;
    std::vector<cf32> a_, b_, c_, clean_;
    cf32 dc_ = cf32(0, 0);
    float dcA_ = 0;
    double pw_ = 0; uint64_t pn_ = 0;
};

}} // namespace dect2::drm
