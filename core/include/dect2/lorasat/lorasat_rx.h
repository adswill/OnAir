// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRa satellite receiver: LoRa frames from (and to) satellites on 400 - 440 MHz and 868 / 915 MHz. Reuses the LoRa physical layer of the mesh receiver
// (mesh_lora.h) with what a satellite adds: a Doppler shift of up to +-10 kHz at 436 MHz and +-20 kHz at 868 / 915 MHz that moves during every pass, and
// a Doppler rate (up to about 150 Hz/s at 436 MHz, 300 Hz/s at 868 MHz) that tilts the chirps through a long frame. The channel filter is widened for the
// shift and the demodulator follows the carrier through the frame (lora::Params::cfoTrack); each good frame gives a point of the Doppler curve.
// The pass planner (app) names the satellite overhead (setTracked) and does NOT retune the radio for this mode: the decoder follows the shift itself.
// Receive only; payloads are shown as bytes (hex and text), nothing is decoded further.
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks), telemetry() and the
// setters from the interface thread.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/lorasat/lorasat_tel.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class LoraSatReceiver {
public:
    LoraSatReceiver();
    ~LoraSatReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the user's dial sits relative to 0 Hz in the input
    void setCenterHz(double hz);                         // the radio's centre frequency (0: not known)
    bool ready() const;
    void reset();
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(LoraSatTelemetry& out, uint64_t lastSeq);
    std::string saveResults() const;
    bool loadResults(const std::string& blob);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setSettings(const LoraSatSettings& s);
    LoraSatSettings settings() const;
    void setTracked(int norad, const std::string& name); // the pass planner: the satellite of the pass (0, "" none)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning loraSatTuning();

} // namespace dect2
