// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ROS (HF) detector mode. ROS (José Alberto Nieto Ros, EA5HVK) is a proprietary weak-signal HF chat mode whose framing, coding and FEC are not
// public, so nothing can be READ from it. This receiver brings the upper sideband to 8 kHz audio (as the other HF modes: 24 kHz front end, USB),
// keeps a waterfall of it, and every two seconds measures the last 16 s with the ROS identifier (ros/ros_detect.h): one tone at a time on a
// fine grid, about 2.25 kHz wide, 4 / 8 / 16 baud. A detection names the sub-mode, the tone range and the grid; the waterfall marks the band.
// Text decoding is "partial, needs a public spec": it stays empty.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/audio/hf_demod.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct RoshfTelemetry;   // roshf_tel.h

class RoshfReceiver {
public:
    RoshfReceiver();
    ~RoshfReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();                        // after a retune: keeps the detection log
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(RoshfTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setDemod(int d);                // HfDemod: USB (default) or LSB; the others count as USB
    int demod() const;
    void flush();                        // tests: measure now
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning roshfTuning();

struct RoshfPreset { const char* name; double mhz; };
const RoshfPreset* roshfPresets(int& count);

} // namespace dect2
