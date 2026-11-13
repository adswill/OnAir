// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DECT monitor: the base stations of cordless telephones on one carrier, from their beacons (identity and system information), and how
// busy the carrier is (bursts from base stations and handsets, slots in use). Receive only; the calls themselves (B-field) are scrambled
// and encrypted and are not decoded, and there is no voice.
//
// 1.152 Mbit/s GFSK. The carrier is mixed to 0 Hz, low-pass filtered and resampled to 4.608 Msps (4 samples per bit), FM-demodulated and
// smoothed over a bit. Every sample phase correlates the last 32 bits against the base-station and handset S-field patterns; a hit gives the
// eye, then the 64-bit A-field is sliced and kept when its CRC passes. Written to the engine's contract (configure, feed, telemetry).
#pragma once
#include "dect_tel.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class DectReceiver {
public:
    DectReceiver();
    ~DectReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(DectTelemetry& out, uint64_t lastSeq);
    void clearResults();
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning dectTuning();

}  // namespace dect2
