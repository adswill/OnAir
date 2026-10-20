// In-memory test signal only. No transmitter is registered for these modes.
#pragma once
#include "railway_proto.h"
#include "dect2/engine/mode_synth.h"
#include <memory>
namespace dect2::railway {
std::vector<uint8_t> demoFrame(Protocol protocol);
std::vector<uint8_t> airBits(const std::vector<uint8_t>& frame, bool inverted = false);
}
namespace dect2 {
std::unique_ptr<ModeSynth> makeRailwaySynth(const SynthConfig& cfg, double rate, railway::Protocol protocol);
}
