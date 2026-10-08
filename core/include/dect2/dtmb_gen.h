// DTMB test signal: the generator the synthetic source plays and dtmbtool writes to a file. It is a transmitter model (dtmb_tx.h) followed by
// the square-root raised cosine filter at any sample rate, an echo channel, a carrier and clock offset and noise. Nothing is transmitted.
//
// SynthConfig fields used: snrDb (carrier to noise in 7.56 MHz, above 150 = no noise), cfoHz, sroPpm, echoDb / echoDelay (one post-echo:
// attenuation in dB, delay in samples of the output rate).
// SynthConfig::modeOpt (all zero = PN945, 64QAM, rate 0.6, interleaver mode 1, demo programme):
//   [0] frame header   0 PN945, 1 PN595, 2 PN420
//   [1] modulation     0 64QAM, 1 32QAM, 2 16QAM, 3 4QAM, 4 4QAM-NR     (32QAM and 4QAM-NR exist at rate 0.8 only: that rate is used)
//   [2] code rate      0 = 0.6, 1 = 0.4, 2 = 0.8
//   [3] interleaver    0 mode 1 (M = 240, 84 ms), 1 mode 2 (M = 720, 253 ms)
//   [4] carriers       0 = C=3780 (multi-carrier); 1 = C=1 (single carrier: always PN595 with a fixed phase, whatever [0] and [5] say)
//   [5] PN phase       0 rotating inside the super-frame, 1 fixed (PN420 and PN945)
//   [6] payload        0 looping test-card programme, 1 numbered test packets (dtmbTestPacket)
//   [7] channel        0 8 MHz (7.56 Msym/s), 1 6 MHz (5.67 Msym/s, Cuba); the app sets it from the receiver's channel width
// SynthConfig::modeVal: [0] second echo, attenuation in dB (0 = off); [1] its delay in samples, negative = arrives before the main path.
#pragma once
#include "dtmb_tx.h"
#include "mode_synth.h"
#include <vector>

namespace dect2 {
std::unique_ptr<ModeSynth> makeDtmbSynth(const SynthConfig& cfg, double sampleRate);   // nullptr: below 8 Msps

namespace dtmb {

struct SignalConfig {
    TxConfig tx;
    double rate = 10e6;            // output sample rate, Hz (8 MHz and up)
    double symbolRate = kSymbolRate;   // symbolRateFor() of the channel: 5.67 Msym/s for a 6 MHz channel
    double snrDb = 40;             // carrier to noise power in 7.56 MHz; above 150 no noise
    double cfoHz = 0;
    double sroPpm = 0;             // the receiver's clock runs this many ppm slow: the signal is read this much faster
    struct Echo { double attenuationDb; double delay; };   // delay in output samples; negative: before the main path
    std::vector<Echo> echoes;
    float rms = 0.22f;             // rms of the output samples (complex)
    uint32_t seed = 1;
};

class Signal {
public:
    Signal(const SignalConfig& cfg, FrameTx::TsSource ts);
    // The next n samples of the endless signal
    void generate(cf32* out, size_t n);
    const SignalConfig& config() const { return cfg_; }
    FrameTx& tx() { return tx_; }
    // Output sample index at which symbol 0 of the first frame sits (the delay of the pulse shaping filter), for tests
    double firstSymbolSample() const { return 0.0; }

private:
    void ensure(long upTo);
    SignalConfig cfg_;
    FrameTx tx_;
    std::vector<float> re_, im_;
    std::vector<cf32> frame_;
    long base_ = 0;                // symbol index of re_[0]
    double tau_ = 0;               // position of the next output sample, in symbols
    double step_ = 1;              // symbols per output sample
    std::vector<float> table_;     // pulse weights: phases x taps
    int phases_ = 0, taps_ = 0;
    std::vector<cf32> pool_, hist_;
    uint32_t rnd_ = 1;
    size_t histPos_ = 0;
    struct Path { double gain; long delay; };
    std::vector<Path> paths_;
    double dph_ = 0;
    cf32 rot_{1, 0}, cur_{1, 0};
    float noiseSigma_ = 0, gain_ = 1;
    long produced_ = 0;
};

// Numbered test packets for bit-exact checks: PID 0x100, packet n carries n and a pseudo-random fill derived from n and the seed.
std::function<void(uint8_t*)> testPacketSource(uint32_t seed);
// True when `pkt` is test packet number n of that seed; if n is not known pass the number found in the packet through *n
bool checkTestPacket(const uint8_t* pkt, uint32_t seed, uint32_t* number);

} // namespace dtmb
} // namespace dect2
