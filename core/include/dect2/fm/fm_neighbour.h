// First-adjacent cancellation for FM: a strong first-adjacent neighbour at +-200 kHz is demodulated with its own limiter, regenerated as a constant-envelope FM waveform and subtracted from
// the capture, so that the wanted station's filter no longer sees its skirts. Automatic: runs only while a neighbour stands at least 10 dB above the wanted station.
// A strong neighbour's FM waveform is recoverable because the wanted station is far below it (the limiter keeps the stronger signal). Filter delays are matched.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/dsp/fftutil.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

class FmNeighbourCancel {
public:
    static constexpr double kMaxRate = 2.6e6;   // above this the FIR cost is not worth it (a wide capture then has the census)
    void configureIf(double rate);              // first call or a new rate
    void reset();
    void process(const cf32* x, size_t n, cf32* out);   // n samples in, n samples out, delayed by nothing visible (the output is x minus the estimate of the neighbour, aligned)
    double activeOffsetHz() const { return offset_; }   // 0: nothing being cancelled
    float neighbourDb() const { return levelDb_; }      // over the wanted station

private:
    void detect(const cf32* x, size_t n);
    double rate_ = 0;
    std::vector<float> taps_;
    std::vector<cf32> hist_;                    // the raw input, for the matched delay
    std::vector<cf32> firIn_;
    size_t pos_ = 0;
    int nt_ = 0, delay_ = 0;
    double offset_ = 0;
    cf32 rot_{1, 0}, rotStep_{1, 0};
    cf32 prevN_{1, 0}, g_{0, 0}, delayRot_{1, 0};
    float ph_ = 0, pAvg_ = 1e-9f, levelDb_ = 0;
    std::unique_ptr<Fft> fft_;
    std::vector<cf32> sp_;
    std::vector<float> pw_;
    std::vector<cf32> scan_;
    size_t fill_ = 0;
    int frames_ = 0, activeHold_ = 0;
};

} // namespace dect2
