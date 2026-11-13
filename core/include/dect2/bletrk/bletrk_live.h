// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// BLE tracker detector for the engine: IQ in, item trackers and unwanted-tracking alerts out.
//
// No DSP of its own: the Bluetooth LE advertising demodulator (ble_rx.h) runs on the samples and hands every good packet here
// (setPacketCallback). Packets that carry a tracker advertisement (bletrk_proto.h: Apple Find My network frames, Tile, Chipolo, Samsung
// SmartTag, Google Find My Device) go into the tracker book (bletrk_book.h), which follows how long and how close each one stays and
// raises an alert for one that stays with the receiver. The alert goes to the automation engine as "ble.tracker" (a watchlist of the Words
// kind catches it) and into the telemetry.
//
// Privacy: identifiers are kept in memory for a short window only (kBletrkForgetSec), never saved, shown as the last two address bytes.
// Receive only: nothing is transmitted and no tracker is contacted.
//
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

struct BletrkTelemetry;   // bletrk_tel.h

class BletrkLiveReceiver {
public:
    BletrkLiveReceiver();
    ~BletrkLiveReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);          // the radio's centre: which advertising channel is tried first (0: unknown, a file)
    bool ready() const;
    void reset();                         // the demodulator only: the tracker book stays
    void clearResults();                  // forgets every tracker and alert
    void feed(const cf32* x, size_t n);
    bool telemetry(BletrkTelemetry& out, uint64_t lastSeq);
    void setAlertSeconds(double sec);     // how long a tracker must stay (default 600 s)
    void setNearDbfs(float dbfs);         // the averaged level counted as near (default -60 dBFS: the radio's level, not calibrated dBm)
    void ignoreTracker(uint32_t id, bool on);   // the user's own tracker: listed, never alerted
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning bletrkTuning();

} // namespace dect2
