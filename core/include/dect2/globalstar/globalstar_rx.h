// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Globalstar activity monitor (L-band 1610 - 1626.5 MHz = the terminal UPLINK, mobile to satellite; 13 CDMA channels of 1.23 MHz. The satellite downlink is S-band, 2483.5 - 2500 MHz, and is not tuned). Receive only: the CDMA signal is spread and
// encrypted, nothing of its content is decoded. What the receiver does: per channel power, noise floor, activity (occupancy over the last
// minute, an event log of activity stretches) and the Doppler shift of every active channel from its spectral centroid, as a track in time.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread, telemetry() and the setters
// from the interface thread. The channel plan is placed from the dial frequency (setDialHz) and the signal offset.
#pragma once
#include "globalstar_tel.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class GlobalstarReceiver {
public:
    GlobalstarReceiver();
    ~GlobalstarReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);       // where the dial frequency sits relative to 0 Hz of the input
    void setDialHz(double hz);             // the frequency tuned (0: unknown, a file: the band centre is assumed)
    void setWallClock(bool on);
    // 4.1: the first floor of a channel that was already on is capped over its neighbours' (default) instead of the quietest channel of the band
    // (false: as in 4.0); takes effect at the next start
    void setLocalFloorCap(bool on);
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(GlobalstarTelemetry& out, uint64_t lastSeq);
    void clearResults();                   // the event log and the Doppler track
    std::string saveResults() const;
    bool loadResults(const std::string& blob);
    void setLogCallback(std::function<void(const std::string&)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning globalstarTuning();

} // namespace dect2
