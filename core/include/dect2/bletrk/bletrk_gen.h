// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// BLE tracker detector test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// A schedule of tracker advertisements (an Apple Find My network frame, Tile, Chipolo, Samsung SmartTag and a Google Find My Device tag,
// at different levels) and a phone that is not a tracker, played by the Bluetooth LE generator (ble_gen.h) on the three advertising channels.
//
// SynthConfig (makeBletrkSynth): snrDb (carrier to noise in 1 MHz), cfoHz; modeOpt[0]: 0 all the advertisers, 1 only the Find My frame.
#pragma once
#include "dect2/ble/ble_gen.h"
#include "dect2/engine/mode_synth.h"
#include <memory>
#include <vector>

namespace dect2 {

// The advertisers; only1: just the Find My frame. channel 0: the packets go round 37, 38, 39.
std::vector<BleGenPacket> bletrkTestSchedule(bool only1 = false, int channel = 0);

std::unique_ptr<ModeSynth> makeBletrkSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
