// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CIS HF modes receiver (families 620 - 626, receive only; see cishf_dsp.h and docs/CIS-HF-3.9.md). One class for the seven kinds: the kind says
// which decoder or identifier runs. The channel is mixed to 0 Hz (the upper sideband dial) and brought to 24 kHz complex (marine::Front), then
// halved to the 12 kHz complex stream the decoders read (audio frequency f of the upper sideband at +f Hz).
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/engine/mode_tuning.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct CisHfTelemetry;   // cishf_tel.h

class CisHfReceiver {
public:
    CisHfReceiver(int kind = 0);   // not explicit: the engine keeps an array {0, 1, ...}
    ~CisHfReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();                        // after a retune
    void feed(const cf32* x, size_t n);
    bool telemetry(CisHfTelemetry& out, uint64_t lastSeq);
    void clearResults();
    void setForced(double baud, double shiftHz);   // Advanced: FSK kinds only, 0 = automatic
    void setLogCallback(std::function<void(const std::string&)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning cisHfTuning(int kind);
// The test signal of a kind (engine standard 622 + kind): looped, at the radio rate, the dial 20 kHz below the centre (tuneOffsetHz).
// modeOpt[0]: the variant of cishf::GenOpts (CIS-12 PSK order, OFDM HDR variant, CIS-36 rate); BEE: 1 = 100 Bd traffic (CIS-50-50).
std::unique_ptr<ModeSynth> makeCisHfSynth(int kind, const SynthConfig& cfg, double sampleRate);

} // namespace dect2
