// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ESP-NOW test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// ESP-NOW frames (espnow_proto.h) through the Wi-Fi test generator (wifi_gen.h): DSSS at 1 Mbit/s (ESP-NOW's default) and 2 Mbit/s, OFDM at 6
// and 12 Mbit/s, with carrier offset, clock error and noise.
//
// SynthConfig (makeEspnowSynth): snrDb, cfoHz; modeOpt[0]: 0 the mixed scene of espnowTestSchedule, 1 only its 1 Mbit/s frames.
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/wifi/wifi_gen.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

// The key of the scene's encrypted sender (00 01 02 ... 0F): enter it under Tools > Keys (protocol ESP-NOW) to see its payload.
extern const uint8_t kEspnowTestKey[16];

// Five senders: a temperature sensor broadcasting text at 1 Mbit/s (24:6F:28:11:22:33), a button board sending a binary struct to a peer at
// 1 Mbit/s (84:CC:A8:44:55:66), a 2 Mbit/s sender with 200 bytes (long preamble) and one with the short preamble (A4:CF:12:01:02:03), an OFDM
// 6 Mbit/s sender and a 12 Mbit/s one (EC:94:CB:0A:0B:0C), an encrypted unicast sender (30:AE:A4:77:88:99, key kEspnowTestKey), a v2 frame of
// 600 bytes, and an access point's beacon that is not ESP-NOW. onlyDsss1: only the 1 Mbit/s frames.
std::vector<WifiGenFrame> espnowTestSchedule(bool onlyDsss1 = false);

std::unique_ptr<ModeSynth> makeEspnowSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
