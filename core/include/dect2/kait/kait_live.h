// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAIT for the engine: IQ in (any rate of 300 ksps or more), the last picture burst in the telemetry, decoded layer by layer.
// The receiver keeps the last 16 s at 632 ksps, looks for an up sweep once a second, and when one is found waits for the rest of the 10 s burst, then
// decodes it on a worker thread (kait_rx.h decodeBurst) and publishes the picture after every layer. Family 131, engine standard 133, telemetry 132.
// Limit: the picture appears after the burst, not during it (the decoder works on the captured burst), and one burst at a time.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread,
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_tuning.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct KaitTelemetry;   // kait_tel.h

class KaitLiveReceiver {
public:
    KaitLiveReceiver();
    ~KaitLiveReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double) {}              // the burst sits on the tuned frequency
    void setCenterHz(double) {}
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(KaitTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void waitIdle();                             // tests: blocks until a running decode has finished

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning kaitTuning();

} // namespace dect2
