// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF utility modes (receive only): MIL-STD-188-110 serial, STANAG 4285 (with STANAG 5066 framing), ARQ-E, ARQ-N, AUTOSPEC, SPREAD, G-TOR, EFR teleswitch
// (DCF49 / DCF39), Piccolo Mk VI and Coquelet-8. One class, the kind says which decoder runs. Written to the engine's contract: configure() once the
// sample rate is known, feed() from the analysis thread, telemetry() and the setters from the interface thread. The decoders are simulated-signal only
// (docs/HF-UTIL-3.8.md lists what each was checked against).
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/hfutil/hfutil_tel.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class HfUtilReceiver {
public:
    HfUtilReceiver(int kind = 0);
    ~HfUtilReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(HfUtilTelemetry& out, uint64_t lastSeq);
    void clearResults();
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setForcedProfile(int p);        // STANAG 4285 only (hfu_serial.h): -1 automatic
    void setForcedFsk(double baud, double shiftHz);   // 0 automatic
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning hfUtilTuning(int kind);
std::unique_ptr<ModeSynth> makeHfUtilSynth(int kind, const SynthConfig& cfg, double sampleRate);

struct HfuPreset { const char* name; double mhz; };
const HfuPreset* hfuPresets(int kind, int& count);

}  // namespace dect2
