// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LBJ (Chinese railway train broadcast) receiver: FM on the railway UHF warning channel (about 821.2375 MHz),
// 1200 bit/s FSK after FM demodulation, BCH-protected frames with train number, kilometre marker and locomotive data.
// Receive only.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread,
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct LbjTelemetry;

class LbjReceiver {
public:
    LbjReceiver();
    ~LbjReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);
    bool ready() const;
    void reset();
    void clearMessages();
    void feed(const cf32* x, size_t n);
    bool telemetry(LbjTelemetry& out, uint64_t lastSeq);
    void clearResults();
    std::string saveResults() const;
    bool loadResults(const std::string& blob);
    void setLogCallback(std::function<void(const std::string&)> cb);
    bool busy() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning lbjTuning();

} // namespace dect2
