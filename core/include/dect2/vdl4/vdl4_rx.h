// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDL Mode 4 receiver: GFSK on VHF channels inside the sample band.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_tuning.h"
#include "vdl4_tel.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

std::vector<double> vdl4EuropeChannels();

class Vdl4Receiver {
public:
    Vdl4Receiver();
    ~Vdl4Receiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(Vdl4Telemetry& out, uint64_t lastSeq);
    void clearResults();
    std::string saveResults() const;
    bool loadResults(const std::string& blob);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setChannels(const std::vector<double>& hz);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning vdl4Tuning();

} // namespace dect2
