// Internet TV (IPTV) mode entry for the engine's tables: family 209, engine standard 211. See dect2/tv/iptv.h and docs/IPTV.md.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/engine/mode_synth.h"
#include <memory>
namespace dect2 {
ModeTuning iptvTuning();
std::unique_ptr<ModeSynth> makeIptvSynth(const SynthConfig& cfg, double sampleRate);   // silence
}
