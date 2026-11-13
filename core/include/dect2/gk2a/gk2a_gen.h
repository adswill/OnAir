// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GK-2A LRIT / HRIT test signals. Nothing is transmitted: the samples only exist in memory or in a file.
//
// Test products (gk2aTestFiles): the full-disk infrared channel IR105 and the visible channel VI006 as a 128 x 96 picture of four
// segments each (10 bits, lossless JPEG 2000 made by j2k::encodeLossless, one file per segment, header 128 with sequence / total /
// first line, VC 0, APID = channel number), an observation-schedule text file (VC 4, APID 128) and an additional-data file (VC 5,
// APID 160). With SynthConfig::modeOpt[0] = 1 the VI006 segments are DES-encrypted with the test key (key number 1, gk2aTestKey()),
// so the receiver shows the "needs a key" state until the key is imported. Then the usual layers: transport file, CP_PDUs, VCDUs of
// 892 bytes, Reed-Solomon (255,223) x4, randomiser, sync word, rate 1/2 K = 7 code (G2 inverted), LRIT: BPSK 128 ksym/s, HRIT: QPSK
// 3 Msym/s, root raised cosine 0.5. snrDb is Es/N0 of a symbol. SynthConfig: snrDb, cfoHz, sroPpm, modeOpt[0] (encrypt VI006).
#pragma once
#include "dect2/goes/goes_gen.h"
#include <array>

namespace dect2 {

std::array<uint8_t, 8> gk2aTestKey();                                 // the DES key of key number 1 in the test signal
std::vector<uint8_t> gk2aTestKeyFile();                               // that key as an EncryptionKeyMessage.bin
std::vector<GoesTestFile> gk2aTestFiles(bool encryptVisible);
// the pixels of a test picture (10 bits, 128 x 96): channel 0 = IR105 (cold clouds bright), 1 = VI006
std::vector<uint16_t> gk2aTestPicture(int channel, int& w, int& h);
constexpr int kGk2aTestSegments = 4;

std::unique_ptr<ModeSynth> makeGk2aLritSynth(const SynthConfig& cfg, double sampleRate);
std::unique_ptr<ModeSynth> makeGk2aHritSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
