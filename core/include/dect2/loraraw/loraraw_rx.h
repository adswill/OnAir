// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRa (raw) monitor: plain LoRa between two radios (point to point, not LoRaWAN and not a mesh): the chirp spread spectrum frames any SX127x / SX126x
// device sends, shown as bytes (hex and text) with level, SNR and carrier offset. Receive only: nothing is sent, nothing is decrypted or interpreted.
//
// Reuses the LoRa physical layer of the mesh receiver (mesh_lora.h): a channel filter per bandwidth and a demodulator per spreading factor and sync
// word. The user fixes the spreading factor (SF5 - SF12), bandwidth (125 / 250 / 500 kHz), coding rate and CRC (implicit header only: an explicit
// header carries them), the header mode and the sync word, or leaves the spreading factor and the bandwidth on Auto and every combination is
// searched at once (more CPU). Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never
// blocks), telemetry() and the setters from the interface thread.
#pragma once
#include "loraraw_tel.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class LoraRawReceiver {
public:
    LoraRawReceiver();
    ~LoraRawReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the user's frequency sits relative to 0 Hz in the input
    bool ready() const;
    void reset();                                        // after a retune: forget the signal (the frame log stays: clearResults)
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(LoraRawTelemetry& out, uint64_t lastSeq);
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setSettings(const LoraRawSettings& s);          // any thread; takes effect at the next feed()
    LoraRawSettings settings() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning loraRawTuning();

} // namespace dect2
