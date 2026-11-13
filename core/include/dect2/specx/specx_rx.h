// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Spectrum explorer: no decoder, only a spectrum of everything the radio sees (Blackman-Harris window, FFT of a chosen size, averaged
// in power), for looking around, measuring carriers and sweeping a range. Engine standard 43 (active 42).
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
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

struct SpecxTelemetry;   // specx_tel.h

class SpecxReceiver {
public:
    SpecxReceiver();
    ~SpecxReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double) {}      // the whole band is shown: nothing to mix out
    bool ready() const;
    void reset();                        // after a retune: the averaging starts again
    void feed(const cf32* x, size_t n);
    bool telemetry(SpecxTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setSilent(bool) {}

    void setFftSize(int n);              // 256 .. 65536, a power of two (default 4096)
    void setAveraging(int n);            // 1 (none) .. 100 spectra (default 4)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning specxTuning();
std::string specxSummary(const SpecxTelemetry& t);

} // namespace dect2
