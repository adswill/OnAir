// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DJI drone test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// makeDjiSynth(): a 2.4 GHz-style capture (>= 16 Msps) with, by scenario (SynthConfig::modeOpt[0]):
//   0  everything: a DroneID burst train 5 MHz below the centre, an OcuSync-like burst train 5 MHz above it, and Wi-Fi-like wideband bursts
//   1  DroneID only    2  OcuSync-like only (10 MHz OFDM, 1 ms bursts every 2 ms)    3  Lightbridge-like only (10 MHz OFDM, 0.7 ms bursts every 1 ms, repeated preamble)
//   4  Wi-Fi-like only (white wideband bursts of random length)    5  DroneID with Wi-Fi-like bursts that collide with it
// modeOpt[1] = 1: the DroneID bursts leave out the first symbol (8 symbols, like the Mavic 2); modeOpt[2] = 1: the OcuSync-like train is 20 MHz wide, centred.
// modeVal[0]: seconds between DroneID bursts (default 0.64). SynthConfig::snrDb: signal to noise over the whole capture; cfoHz: offset of every signal.
// The DroneID bursts follow the published structure (dji_proto.h). The OcuSync-like and Lightbridge-like trains are NOT the real protocols (those are not
// public): they are OFDM burst trains with the shape described for them (10 / 20 MHz, 15 kHz spacing, periodic frames), made to exercise the shape detector.
#pragma once
#include "dji_proto.h"
#include "dect2/engine/mode_synth.h"
#include <memory>
#include <string>

namespace dect2 {

constexpr const char* kDjiGenSerial = "OnAirTestDrone01";   // the serial number the test signal sends (16 characters)
constexpr double kDjiGenDroneIdMhz = -5.0, kDjiGenTrainMhz = 5.0;   // where the trains sit relative to the centre
std::unique_ptr<ModeSynth> makeDjiSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
