// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct SpecanTelemetry;   // specan_tel.h

// Spectrum analyser: sweeps loHz .. hiHz. With the HackRF's native sweep (SaSweepTap active) it takes the tagged blocks; otherwise it
// steps the radio over the range through the retune callback (0.75 x the sample rate per step), or, with no callback (a file), shows
// the band around the centre.
class SpecanReceiver {
public:
    SpecanReceiver();
    ~SpecanReceiver();
    void configure(double inputRateHz);
    void setCenterHz(double hz);             // where the radio is (the stepped sweep starts from its own plan)
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(SpecanTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setRetuneCallback(std::function<bool(double centreHz)> cb);   // the engine retunes the radio (stepped sweep)
    void setSilent(bool) {}

    void setRange(double loHz, double hiHz);
    void range(double& loHz, double& hiHz) const;
    void setBinHz(double hz);                // resolution: the FFT bin width (the grid follows)
    void setAveraging(int n);                // sweeps in the averaged trace
    void setRemoveSpurs(bool on);
    void resetPeak();
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning specanTuning();

} // namespace dect2
