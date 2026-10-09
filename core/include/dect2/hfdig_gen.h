// HF digital test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// makeHfdigSynth(): one upper sideband transmission on the user's (dial) frequency, at -tuneOffsetHz from 0 Hz in the samples, over
// low-level complex noise (about -31 dBFS). The transmission is the test audio of one decoder (8000 Hz, -1 .. 1), turned into its
// upper sideband (200 .. 3800 Hz above the dial frequency) and brought to the sample rate. Each decoder makes its own test audio in its
// own file through the factories below; while a factory returns nullptr the generator sends the noise alone.
//
// SynthConfig:
//   snrDb       signal to noise ratio in 3 kHz, for test audio with an rms of 0.35 (a sine of amplitude 0.5); louder audio gives more
//   cfoHz       carrier offset (the audio moves up by it)
//   modeOpt[0]  which test signal: 0 RTTY, 1 SSTV, 2 FreeDV
//   modeOpt[1..7], modeVal[0..3]: free for the chosen decoder's test audio (only one plays at a time)
#pragma once
#include "mode_synth.h"
#include <cstddef>
#include <memory>

namespace dect2 {

std::unique_ptr<ModeSynth> makeHfdigSynth(const SynthConfig& cfg, double sampleRate);

// One decoder's test transmission as audio: 8000 Hz, -1 .. 1, endless.
class HfdigTestAudio {
public:
    virtual ~HfdigTestAudio() = default;
    virtual void generate(float* out, size_t n) = 0;
};

// Defined in each decoder's own file (hfdig_rtty.cpp, hfdig_sstv.cpp, hfdig_freedv.cpp); nullptr = no test signal yet.
std::unique_ptr<HfdigTestAudio> makeRttyTestAudio(const SynthConfig& cfg);
std::unique_ptr<HfdigTestAudio> makeSstvTestAudio(const SynthConfig& cfg);
std::unique_ptr<HfdigTestAudio> makeFreedvTestAudio(const SynthConfig& cfg);
// The one modeOpt[0] picks (0 RTTY, 1 SSTV, 2 FreeDV; anything else: nullptr)
std::unique_ptr<HfdigTestAudio> makeHfdigTestAudio(int which, const SynthConfig& cfg);

} // namespace dect2
