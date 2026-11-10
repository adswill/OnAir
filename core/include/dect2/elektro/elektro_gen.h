// Elektro-L HRIT test signal. Nothing is transmitted: the samples only exist in memory or in a file.
// The framing is the GOES / CGMS xRIT test products (goes_gen.h). SynthConfig modeOpt[0]: the link as ElektroReceiver::setLink numbers it
// (0 HRIT 665.4 ksym/s, the default; 1 HRIT 1 Msym/s; 2 LRIT 128 ksym/s; 3 LRIT 64 ksym/s; 5 294 ksym/s; 6 HRIT 1.15 Msym/s OQPSK).
#pragma once
#include "dect2/engine/mode_synth.h"
#include <memory>

namespace dect2 {

std::unique_ptr<ModeSynth> makeElektroSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
