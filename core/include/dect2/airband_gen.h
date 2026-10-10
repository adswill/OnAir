// Airband test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// Several AM channels around the dial frequency (which sits at -tuneOffsetHz in the samples, as the radio is tuned above it), each with
// its own level, transmitter offset, modulation and keying times, over complex white noise. The audio is voice-like (band-limited noise
// 300 - 2800 Hz with a syllable rhythm) or a 1 kHz tone for measuring the audio SNR; the carrier comes up and goes down in 2 ms.
//
// SynthConfig (makeAirbandSynth):
//   snrDb       carrier to noise of a 0 dB channel, the noise taken in 6.8 kHz (the receiver's measure for an 8.33 kHz channel)
//   cfoHz       the radio's tuning error: every channel moves by it
//   modeOpt[0]  layout: 0 the test layout (airbandTestLayout), 1 one 8.33 channel at the dial frequency with a tone, keyed 2 s in 5 s
#pragma once
#include "mode_synth.h"
#include "airband_rx.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct AirbandGenChannel {
    double offsetHz = 0;         // from the dial frequency
    bool is833 = false;          // only for the name and the channel list (the signal is the same)
    std::string label;
    double levelDb = 0;          // carrier power relative to the 0 dB channel
    double cfoHz = 0;            // the transmitter's own offset
    double mod = 0.8;            // modulation depth (peaks); above 1 the transmitter clips the envelope at zero (overmodulation)
    int audio = 0;               // 0 voice-like, 1 a 1 kHz tone, 2 carrier only
    bool continuous = false;     // always on (ATIS); otherwise keyed as below
    double period = 6, on = 2, phase = 0;   // keyed on from phase + k * period for on seconds (k = 0, 1, ...)
    std::vector<std::pair<double, double>> bursts;   // instead of the period, when not empty: (start, length) in seconds
};

struct AirbandGenConfig {
    double rate = 2e6;
    double dialOffsetHz = 0;     // where the dial frequency sits in the samples (-tuneOffsetHz for the radio, 0 for a recording)
    double snrDb = 30;           // as SynthConfig::snrDb; >= 99: no noise
    double cfoHz = 0;            // the radio's tuning error
    double amp = 0.05;           // carrier amplitude of a 0 dB channel
    uint32_t seed = 1;
    std::vector<AirbandGenChannel> chans;
};

class AirbandGenerator {
public:
    explicit AirbandGenerator(const AirbandGenConfig& c);
    ~AirbandGenerator();
    void generate(cf32* out, size_t n);
    double timeSec() const;                              // signal time of the next sample
    bool keyed(int chan, double t) const;                // channel chan transmits at signal time t
    static double noiseSigma(const AirbandGenConfig& c); // per component
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The test layout: tower (25 kHz, at the dial), approach (8.33, one step up), ground (25 kHz, +250 kHz), ATIS (25 kHz, -300 kHz,
// continuous), and a tone channel (8.33, +191.667 kHz) with a transmitter 300 Hz off.
std::vector<AirbandGenChannel> airbandTestLayout();
// The channel list that matches a layout for a dial frequency (the app's first list, the tests)
std::vector<AirbandChannel> airbandChannelsFor(const std::vector<AirbandGenChannel>& layout, double dialHz);

std::unique_ptr<ModeSynth> makeAirbandSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
