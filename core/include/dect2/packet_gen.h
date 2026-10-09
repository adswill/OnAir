// APRS / Packet test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// makePacketSynth(): skeleton. Low-level complex noise only (about -31 dBFS), so that the synthetic source runs in this mode; the mode's
// worker replaces it with APRS stations sending AX.25 frames.
//
// SynthConfig: not used yet.
#pragma once
#include "mode_synth.h"
#include <memory>

namespace dect2 {

std::unique_ptr<ModeSynth> makePacketSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
