// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Test signal of the Sub-GHz sensors mode: OOK sensors of the Nexus type (PPM, 36 bits sent 12 times: temperature, humidity, battery, channel)
// and now and then one burst of a PWM format that no decoder knows (for the unknown signal analyser). Nothing is transmitted.
// SynthConfig (makeSensorsSynth): snrDb is the SNR in the 250 kHz channel, cfoHz the carrier offset; modeOpt[0] = 1 sends no unknown bursts.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace dect2 {

typedef std::vector<std::pair<double, double>> PulseList;   // (pulse us, gap us)

// the pulse train of one Nexus-TH message (36 bits) repeated `repeats` times; tempC in tenths of a degree
PulseList sensorsNexusPulses(int id, int channel, bool batteryOk, double tempC, int humidity, int repeats = 12);
// a PWM burst (280 / 820 us pulses, 610 us gap) of the given bits, repeated: not any protocol
PulseList sensorsUnknownPwmPulses(uint32_t bits, int nbits, int repeats = 6);
// the pulse list as an OOK signal: complex samples, carrier amplitude amp, rise and fall of 6 us, no noise
std::vector<cf32> sensorsOokSignal(const PulseList& p, double rate, double amp, double leadUs = 3000, double tailUs = 12000);

std::unique_ptr<ModeSynth> makeSensorsSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
