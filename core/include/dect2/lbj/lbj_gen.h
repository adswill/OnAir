// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LBJ test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// makeLbjSynth(): an endless loop of LBJ bursts (576-bit preamble, POCSAG 1200 bit/s, FM with about 4.2 kHz deviation) with fake train data, half a
// second of silence between them. The carrier sits lbjTuning().tuneOffsetHz below the radio's centre plus SynthConfig::cfoHz. snrDb is carrier power
// over noise power in 25 kHz.
//
// SynthConfig::modeOpt:  [1] 1 = polarity reversed   [3] noise seed, 0 = 1
//                        [4] frequency deviation in Hz (0 = 4200)
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/lbj/lbj_pocsag.h"
#include <memory>
#include <string>
#include <vector>

namespace dect2::lbj {

// A page to send: RIC, function, numeric text (digits, ' ', '-', '*', 'U', ')', '(' ; padded with spaces to a multiple of 5).
struct GenPage {
    uint32_t ric = 0;
    int function = 0;
    std::string digits;
};

// The full bit stream of one transmission: preamble, then batches (sync word + 16 codewords), the pages placed in their frames, idle words elsewhere.
std::vector<uint8_t> burstBits(const std::vector<GenPage>& pages, int preambleBits = 576);
// The codewords alone (16 per batch), as the modem hands them over
std::vector<uint32_t> burstWords(const std::vector<GenPage>& pages);
// The numeric text a basic message carries (formatBasic) as a page
GenPage basicPage(const std::string& train, int speedKmh, int kmTenths, int function = 1);

struct TestMessage {
    std::string train, speed, km, direction;
};
// What one cycle of the test signal carries (the short messages)
std::vector<TestMessage> testMessages();

} // namespace dect2::lbj

namespace dect2 {
std::unique_ptr<ModeSynth> makeLbjSynth(const SynthConfig& cfg, double sampleRate);
}
