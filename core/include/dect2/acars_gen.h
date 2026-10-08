// ACARS test signal: aircraft on several VHF channels, downlinks (OOOI, position, weather request, free text, link test, media
// advisory) and ground uplinks (acknowledgements, weather text, clearance text), AM with the 2400 bit/s MSK audio, a pre-key, and
// random timing in which bursts on one channel never overlap. Synthetic content: the texts are made up in the style of real ones.
//
// Through the engine (SynthConfig, mode 18), the options are:
//   modeOpt[0]  number of aircraft, 0 = 6 (1 to 24)
//   modeOpt[1]  random seed, 0 = 1
//   modeOpt[2]  AM depth in percent, 0 = 60 (10 to 95)
//   modeOpt[3]  channels: 0 = 131.525, 131.725 and 131.825 MHz (the Europe set); 1 = 131.525 only; 2 = 131.125, 131.550 and 131.725
//   modeVal[0]  message rate factor, 0 = 1 (every aircraft sends a block about every 15 s divided by this)
//   modeVal[1]  extra attenuation of the last channel of the set in dB, 0 = none (a weak channel next to a strong one)
// snrDb is the carrier-to-noise ratio of a channel at its nominal level, in a 7 kHz band. cfoHz and sroPpm shift every channel.
// The centre frequency of the test signal is 131.5 MHz (the signal is placed relative to it), tuneOffsetHz is 0.
#pragma once
#include "mode_synth.h"
#include "acars_proto.h"
#include <memory>
#include <mutex>
#include <vector>

namespace dect2 {

struct AcarsSent {                 // one transmission as the generator made it (the answer key for the tests)
    double startSec = 0, endSec = 0;
    double freqHz = 0;
    AcarsBlockSpec spec;
};

struct AcarsSentLog {              // shared with the generator, which appends from the thread that calls generate()
    std::mutex mu;
    std::vector<AcarsSent> sent;
};

struct AcarsGenOptions {
    int aircraft = 6;
    uint32_t seed = 1;
    double depth = 0.6;                         // AM depth, 0.1 to 0.95
    double rateFactor = 1.0;
    double centerHz = 131.5e6;
    std::vector<double> channelsHz = {131.525e6, 131.725e6, 131.825e6};
    std::vector<double> levelDb;                // per channel, relative to the nominal level (missing = 0)
    double cfoSpreadHz = 300;                   // each aircraft's carrier sits up to this far from its channel centre
    double guardSec = 0.15;                     // quiet time between two bursts on a channel
    bool uplinks = true;                        // ground replies
    bool positions = false;                     // the first six aircraft also send position reports (ADS-C B6 / H1, POS and label 16 text), see acars_gen.cpp
                                                // (off by default: the extra bursts change the timing the older tests were written for; the engine path turns it on)
    std::shared_ptr<AcarsSentLog> log;
};

std::unique_ptr<ModeSynth> makeAcarsSynthEx(const AcarsGenOptions& o, const SynthConfig& cfg, double sampleRate);
std::unique_ptr<ModeSynth> makeAcarsSynth(const SynthConfig& cfg, double sampleRate);

// The bits on the air for a frame (least significant bit of each byte first) -> the tone of each bit: 2400 Hz when the bit equals the
// one before it, 1200 Hz when it differs (derived from the demodulator of acarsdec msk.c; a steady 2400 Hz is the all-ones pre-key).
std::vector<uint8_t> acarsFrameBits(const std::vector<uint8_t>& bytes);

} // namespace dect2
