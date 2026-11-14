// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Robust Packet (RPR) receiver (receive only, physical layer; see rpr_dsp.h and docs/rpr.md).
// The channel is mixed to 0 Hz and brought to 24 kHz complex (marine::Front), the upper sideband becomes 8 kHz audio (hfdig::UsbAudio), the last 30 s are
// analysed every few seconds on a worker thread. Written to the engine's contract: configure(), feed() never blocks, telemetry() from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct RprTelemetry;

class RprReceiver {
public:
    RprReceiver();
    ~RprReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(RprTelemetry& out, uint64_t lastSeq);
    void clearResults();
    void setLogCallback(std::function<void(const std::string&)> cb);
    void flush();                         // tests: analyse what is buffered now and wait for it
    void setAnalysisInterval(double sec); // tests (default 4 s)
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning rprTuning();

} // namespace dect2
