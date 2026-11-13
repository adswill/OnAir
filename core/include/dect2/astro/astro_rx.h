// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radio astronomy tool (hydrogen line): no decoder, a long integration of the power spectrum (Hann FFT of 1024 .. 16384 bins, averaged
// over 1 s .. 1 h of samples, then again), with optional frequency switching (the radio alternates between its frequency and one shifted
// by a set amount, each phase integrated on its own). The processing (on - off, baseline, velocity, peak) is in astro_dsp.h.
// Engine standard 50 (active 49). Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis
// thread (never blocks), telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct AstroTelemetry;   // astro_tel.h

class AstroReceiver {
public:
    AstroReceiver();
    ~AstroReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double) {}
    bool ready() const;
    void reset();                        // the integration starts again (after a retune too)
    void feed(const cf32* x, size_t n);
    bool telemetry(AstroTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setSilent(bool) {}

    void setFftSize(int n);              // 1024 .. 16384, a power of two (default 4096); restarts the integration
    void setIntegration(double sec);     // 1 .. 3600 s of samples (default 10)
    // frequency switching: every periodSec the radio moves between its frequency and +offsetHz (through the retune callback, which the
    // engine sets only for a radio); off: one phase
    void setFreqSwitch(bool on, double offsetHz, double periodSec = 1.0);
    void setRetuneCallback(std::function<bool(double offsetHz)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning astroTuning();
std::string astroSummary(const AstroTelemetry& t);

} // namespace dect2
