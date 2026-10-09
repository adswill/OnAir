// HD Radio test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// makeHdrSynth(): skeleton. Low-level complex noise only (about -31 dBFS), so that the synthetic source runs in this mode; the mode's
// worker replaces it with hybrid FM and AM stations with their digital sidebands.
//
// SynthConfig: not used yet.
#pragma once
#include "mode_synth.h"
#include <memory>

namespace dect2 {

std::unique_ptr<ModeSynth> makeHdrSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
