// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OpenSky identifier. OpenSky (M/A-COM / Harris) is a 4-slot TDMA system, 9600 baud 4-level FSK in a 25 kHz channel on 700 / 800 / 900 MHz, with no public air interface description; its traffic is encrypted. Nothing is decoded here: the mode measures the symbol rate, the levels, the width and the TDMA burst pattern and says whether the signal fits.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_tuning.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct OpenSkyTelemetry;

class OpenSkyReceiver {
public:
    OpenSkyReceiver();
    ~OpenSkyReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(OpenSkyTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning openSkyTuning();

} // namespace dect2
