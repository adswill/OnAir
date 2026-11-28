// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CATS receiver (receive only): the channel is mixed to 0 Hz and averaged down to 76.8 kHz, the last 1.5 s are searched for packets every 0.5 s
// (cats::demodulate), repeats of one packet are dropped. Written to the engine's contract: configure(), feed() never blocks, telemetry() from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "dect2/engine/mode_synth.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct CatsTelemetry;

class CatsReceiver {
public:
    CatsReceiver();
    ~CatsReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(CatsTelemetry& out, uint64_t lastSeq);
    void clearResults();
    void setLogCallback(std::function<void(const std::string&)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning catsTuning();
// Test signal: a packet (identification, time, GPS, comment) every 2 s at 9600 bd. SynthConfig: snrDb in the 9.6 kHz channel, cfoHz the tuning error.
std::unique_ptr<ModeSynth> makeCatsSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
