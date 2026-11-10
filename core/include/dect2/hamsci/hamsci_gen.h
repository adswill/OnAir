// Test signals of the ham science modes (nothing is transmitted). SynthConfig: snrDb is the signal to noise ratio in 2500 Hz for the audio modes (a value of 99
// or more means 30 dB), cfoHz a tuning error of the audio tones. modeOpt[0] chooses a variant: ISCAT 0 = A, 1 = B; Throb 0 .. 5 = Throb1 .. ThrobX4;
// FSK441 / PI4 / QRSS / DFCW: not used. SuperSID: a flare (+3 dB step on NAA) at 17 minutes when modeOpt[0] = 1. Pulsar: modeVal[0] is the pulse S/N over the whole
// band (default 0.1); the pulsar is B0329+54 (period 0.714520 s, DM 26.76) at 408 MHz and the synthetic pulses are cut at the 5 s block edges.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <memory>

namespace dect2 {

std::unique_ptr<ModeSynth> makeHamsciSynth(const SynthConfig& cfg, double sampleRate, int kind);

}  // namespace dect2
