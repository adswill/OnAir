// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sferics / lightning receiver: the radio impulses of lightning discharges (atmospherics), seen as short wideband bursts above the noise floor.
// Counts strikes and flashes, strikes per minute, a 60 minute activity histogram, and the time of every strike on the sample clock.
// Mains interference (a regular impulse train at 100 / 120 Hz) is recognised and left out of the count; impulses longer than 5 ms are not lightning.
// One receiver sees when and how strong, not where: a position needs the arrival times from several stations (a network such as Blitzortung).
// Engine standard 245, active standard 244, family 243.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/sferics/sferics_tel.h"
#include <complex>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class SfericsReceiver {
public:
    SfericsReceiver();
    ~SfericsReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);          // not used: the impulses are wideband
    void setCenterHz(double hz);
    bool ready() const;
    void reset();
    void feed(const std::complex<float>* x, size_t n);
    bool telemetry(SfericsTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

    void setThresholdDb(double db);           // 6 to 30 dB over the noise floor (default 10)
    void setMainsFilter(bool on);             // recognise and ignore a 100 / 120 Hz impulse train (default on)
    void setMergeMs(double ms);               // pulses closer than this are one strike (default 0.5 ms, 0.1 to 5)
    void clearCounts();                       // zero the counters and lists, keep the noise floor

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning sfericsTuning();
std::string sfericsSummary(const SfericsTelemetry& t);

}  // namespace dect2
