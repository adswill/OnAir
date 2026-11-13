// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal workbench receiver: a level meter on the radio's samples. The analysis itself is done on recordings by the classes of wb_*.h,
// which need no radio and no engine.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct WbTelemetry;   // wb_tel.h

class WbReceiver {
public:
    WbReceiver();
    ~WbReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double) {}
    void setCenterHz(double) {}
    bool ready() const { return true; }
    void reset();
    void clearResults() {}
    void feed(const cf32* x, size_t n);
    bool telemetry(WbTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)>) {}
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning wbTuning();

} // namespace dect2
