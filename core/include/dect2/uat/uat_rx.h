// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#pragma once
#include "uat_tel.h"
#include "dect2/engine/mode_tuning.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class UatReceiver {
public:
    UatReceiver();
    ~UatReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz) {}   // UAT is wideband; offset mixing is in the engine front-end
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(UatTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setReference(double latDeg, double lonDeg);
    void clearReference();
    void setExpiry(double seconds);
    void clearResults();
    std::string saveResults() const;
    bool loadResults(const std::string& blob);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning uatTuning();

} // namespace dect2
