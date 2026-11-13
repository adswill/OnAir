// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRa (raw) test signal: plain LoRa frames at several settings (SF7 - SF12, 125 / 250 / 500 kHz, coding rates 4/5 - 4/8, with and without CRC, the
// low data rate optimisation at SF11 / SF12 at 125 kHz), one after the other, over complex white noise. Nothing is transmitted: the samples only exist
// in memory or in a file. Uses the LoRa modulator of mesh_lora.h (TxFrame).
//
// SynthConfig (makeLoraRawSynth): snrDb = SNR of the 125 kHz frames in their bandwidth (the others follow from the same noise: a wider LoRa band at
// the same amplitude has a lower SNR); cfoHz added to every frame; sroPpm = the transmitter's clock error.
//   modeOpt[0]  0 the scene (explicit header, sync word 0x12), 1 implicit header: SF8, 250 kHz, 4/6, 12 bytes, CRC on (the receiver must be set the same)
//   modeOpt[1]  sync word (0 = 0x12)
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/mesh/mesh_lora.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct LoraRawGenFrame {
    lora::Params p;
    std::vector<uint8_t> payload;
    double gapSec = 0.15;        // silence after it
    double snrOffDb = 0;         // added to the configured SNR (of the 125 kHz reference)
    double cfoHz = 0;            // this transmitter's own carrier offset, on top of the config's
};

struct LoraRawGenConfig {
    double rate = 2e6;
    double dialOffsetHz = 0;     // where the channel sits in the samples
    double snrDb = 20;           // SNR of a 125 kHz frame in its bandwidth; >= 99: no noise
    double cfoHz = 0;
    double sroPpm = 0;
    double amp = 0.3;            // amplitude of the frames at the reference SNR
    double leadSec = 0.05;
    bool loop = true;
    uint32_t seed = 1;
    std::vector<LoraRawGenFrame> frames;
};

class LoraRawGenerator {
public:
    explicit LoraRawGenerator(const LoraRawGenConfig& c);
    ~LoraRawGenerator();
    void generate(cf32* out, size_t n);
    double endSec() const;       // when the last scheduled frame ends (a non-looping schedule: the signal is over)
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The scene: nine frames at different settings, the payloads are text ("OnAir LoRa raw test SF9 250k")
std::vector<LoraRawGenFrame> loraRawTestSchedule(int sync = 0x12);
// The implicit-header scene: one setting (SF8, 250 kHz, 4/6, CRC), 12 byte payloads, a few different ones
std::vector<LoraRawGenFrame> loraRawImplicitSchedule(int sync = 0x12);

std::unique_ptr<ModeSynth> makeLoraRawSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
