// ATSC 8-VSB test signal generator: transport stream -> fields -> 8-level symbols -> VSB baseband (channel centred) with pilot,
// multipath, carrier offset, noise and sample-rate offset. Never transmitted; used for tests and the built-in synthetic source.
#pragma once
#include "atsc.h"
#include "ring.h"
#include <functional>
#include <random>
#include <vector>
#include <cstdint>

namespace dect2 {
namespace atsc {

struct Echo {
    double delayUs = 0;      // may be negative: a pre-echo (the main path is delayed instead)
    double gainDb = -10;     // relative to the main path (negative = weaker)
    double phaseDeg = 0;
};

struct ChannelConfig {
    double snrDb = 99;       // data power over noise power in 5.38 MHz
    double cfoHz = 0;        // carrier offset
    double sroPpm = 0;       // sample clock offset of the receiver
    std::vector<Echo> echoes;
};

class Generator {
public:
    using PacketSource = std::function<void(uint8_t* pkt188)>;
    // `outRate`: output sample rate in Hz (complex, centred on the channel)
    Generator(PacketSource src, const ChannelConfig& cfg, double outRate, unsigned seed = 1);
    void generate(size_t n, std::vector<cf32>& out);   // appends n samples
    const ChannelConfig& config() const { return cfg_; }
    void setConfig(const ChannelConfig& c) { cfg_ = c; }
    uint64_t fieldsGenerated() const { return fields_; }

private:
    void makeField();             // appends one field of channel-impaired samples at 2 samples/symbol to hi_
    PacketSource src_;
    ChannelConfig cfg_;
    double outRate_;
    std::mt19937 rng_;
    FieldEncoder enc_;
    std::vector<float> rrc_;      // transmit pulse at 2 samples per symbol
    std::vector<cf32> acc_;       // overlap-add accumulator (pulse tails of the previous field)
    std::vector<cf32> hi_;        // finished samples at 2 samples/symbol
    std::vector<cf32> echoHist_;
    uint64_t hiBase_ = 0;         // absolute index of hi_[0]
    uint64_t symIndex_ = 0, fields_ = 0;
    double cfoPhase_ = 0;
    double outPos_ = 0;           // next output position in units of hi samples
    double noiseSigma_ = 0;
    std::vector<float> sinc_;     // windowed sinc table for the output resampler
};

// Taps of the ATSC root-raised-cosine pulse (excess bandwidth 11.52%, Nyquist frequency of the complex channel: symbol rate / 4)
// at `rate` samples per second, `span` pulse lengths each side (unit passband gain when multiplied by 1/rate)
std::vector<float> rrcTaps(double rate, int halfLen);
double rrcValue(double t);   // continuous pulse value with H(0) = 1

} // namespace atsc
} // namespace dect2
