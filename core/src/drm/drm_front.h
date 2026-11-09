// DRM receiver front end: any input rate to the 48 kHz complex baseband the OFDM receiver works on (DRM+, mode E: 192 kHz). Private to the DRM receiver.
#pragma once
#include "dect2/dsp/exact_resampler.h"
#include "dect2/common/ring.h"
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
    // false: the rate is below minIn (default: the output rate). outRate 48 kHz keeps +-22 kHz, 192 kHz (DRM+) +-60 kHz.
    bool configure(double inRate, double outRate = kOutRate, double minIn = 0);
    double outRate() const { return outRate_; }
    bool ready() const { return ready_; }
    void reset();
    // Appends the output samples of the n input samples (DC removed) to out
    void process(const cf32* in, size_t n, std::vector<cf32>& out);
    double levelDbfs() const;             // mean power of the 48 kHz stream since the last call, in dB re full scale
    void resetLevel() { pw_ = 0; pn_ = 0; }
    // Optional channel filter on the 48 kHz output: keeps +-passHz around the middle, 0 = off (the default: the whole +-22 kHz stays).
    // Narrower than the usual 20 kHz channel it takes out the neighbouring stations; a constant delay of kChTaps / 2 samples. Not for the DRM+ lane.
    void setChannelPass(double passHz);
    double channelPass() const { return chPass_; }
    static constexpr int kChTaps = 161;
private:
    double chPass_ = 0;
    std::vector<float> chH_;
    std::vector<cf32> chHist_, chTmp_;
    double inRate_ = 0, outRate_ = kOutRate;
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
