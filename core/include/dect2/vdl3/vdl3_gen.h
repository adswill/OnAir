// VDL Mode 3 test signal: D8PSK bursts in the formats of ICAO Annex 10 Vol III 6.4.4 (training sequences S1, S1*, S2, S2*, Golay coded
// management bursts, V/D bursts with a Reed-Solomon (72,62) data word or stand-in voice bits). Nothing is transmitted; the samples only
// exist in memory or in a file. Synthetic content: the slot timing (an M burst then a V/D burst in each 30 ms slot, 4 slots per 120 ms
// frame) and the values of the system data and header bits are made up, because their definitions are in ICAO Doc 9805, which is not
// public. The voice bursts carry random bits in place of vocoder frames.
//
// Through the engine (SynthConfig, mode 642):
//   modeOpt[1]  random seed, 0 = 1
//   modeOpt[2]  1 = an adjacent ground station 25 kHz above, at the same level
//   modeVal[0]  carrier drift in Hz per second (0 = none)
//   modeVal[1]  where the channel sits relative to the centre, Hz (0 = on the centre)
// snrDb: Es/N0 of the ground station's bursts (signal power over the noise in 10.5 kHz). cfoHz and sroPpm apply to every burst.
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/vdl3/vdl3_proto.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct Vdl3TxBurst {
    double t = 0;                      // seconds: the centre of the first training symbol
    Vdl3Kind kind = Vdl3Kind::Unknown;
    std::vector<uint32_t> info12;
    std::vector<uint8_t> data;         // V/D data
    std::vector<uint8_t> voice;        // V/D voice: 576 bits
    double levelDb = 0;                // relative to the ground station
    double carOffHz = 0;               // the transmitter's own offset
    double chanHz = 0;                 // channel relative to the receiver's channel (adjacent station: +25 kHz)
    std::vector<uint8_t> bits() const; // training + scrambled payload
};

struct Vdl3GenOptions {
    uint32_t seed = 1;
    double aircraftSpreadHz = 300;     // each aircraft's own offset, up to this
    double aircraftLevelDb = -6;       // aircraft bursts this much below the ground station
    bool adjacent = false;             // a second ground station 25 kHz above
    std::string text = "VDL3 TEST";    // data bursts carry this text, then a counter
};

// The traffic alone (tests): bursts in time order, slot after slot
class Vdl3Traffic {
public:
    explicit Vdl3Traffic(const Vdl3GenOptions& o);
    Vdl3TxBurst next();
private:
    Vdl3GenOptions o_;
    uint32_t rng_;
    uint64_t slot_ = 0;
    int phase_ = 0;                    // 0: the slot's M burst, 1: its V/D burst, 2: the adjacent station
    double vdAt_ = 0;
    uint32_t counter_ = 0;
    uint32_t rnd();
};

// Renders one burst into out[0 .. n) where out[0] is input sample outStart; startSample: the first training symbol's position, rate the
// sample rate the symbols are timed at (with a sample rate offset, the generator's effective rate)
void vdl3Render(const std::vector<uint8_t>& bits, cf32* out, int64_t outStart, size_t n, double rate, double startSample, double carHz, double amp, double phase);
double vdl3BurstSamples(size_t bits, double rate);   // from the first training symbol to the end of the tail

std::unique_ptr<ModeSynth> makeVdl3Synth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
