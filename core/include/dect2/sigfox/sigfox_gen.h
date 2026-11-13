// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sigfox test signal. Nothing is transmitted: the samples only exist in memory or in a file. Made-up devices (the IDs are not registered ones),
// made-up authentication bytes (a real code needs the device key), the frame layer of sigfox_proto.h.
//
// makeSigfoxSynth(): an endless sequence of uplink messages, each sent as three frames (ranks 1, 2, 3) one after the other on different
// frequencies within +-80 kHz of the dial. The dial sits sigfoxTuning().tuneOffsetHz below the radio's centre plus SynthConfig::cfoHz.
// snrDb is the carrier power over the noise power in 100 Hz.
//
// SynthConfig::modeOpt:  [0] 1 = 600 bit/s (RC2 / RC4) instead of 100 bit/s   [1] 1 = one frame per message instead of three   [3] noise seed, 0 = 1
// SynthConfig::modeVal:  [0] carrier drift during each frame, Hz per second   [1] seconds of silence between messages (0 = 1 s)
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/ism/ism_sigfox.h"
#include <memory>
#include <vector>

namespace dect2::sigfox {
struct TestDevice { uint32_t id; std::vector<uint8_t> payload; bool singleBit, bitValue, downlinkRequest; };
std::vector<TestDevice> testDevices();   // the messages of the test signal, in the order it plays them
}

namespace dect2 {
std::unique_ptr<ModeSynth> makeSigfoxSynth(const SynthConfig& cfg, double sampleRate);
}
