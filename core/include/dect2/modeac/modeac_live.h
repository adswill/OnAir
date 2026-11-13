// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode A / C receiver for the engine: IQ in, transponder replies out, beside the Mode S / ADS-B messages of the same capture.
// The ADS-B receiver (adsb_rx.h) runs on the same samples, so the aircraft it knows are on the map and a Mode A squawk or a Mode C altitude can
// be matched to them; its decoded messages also tell the reply detector where Mode S messages were on the air (a Mode S message is not a reply).
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/modeac/modeac_dsp.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct ModeAcTelemetry;   // modeac_tel.h

class ModeAcReceiver {
public:
    ModeAcReceiver();
    ~ModeAcReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);            // not used: the channel is on the centre
    bool ready() const;
    void reset();                               // after a retune: the detector and the Mode S front end; the code table stays
    void clearResults();                        // empties the code table and the aircraft
    void setReference(double latDeg, double lonDeg);   // the receiver's position, for the Mode S positions on the map
    void setConfirm(int sightings);             // a code is shown after this many replies (1 to 5, default 2)
    void setExpiry(double seconds);             // a code that has been silent this long leaves the table (default 60)
    void feed(const cf32* x, size_t n);
    bool telemetry(ModeAcTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setReplyCallback(std::function<void(const ModeAcDemod::Raw&)> cb);   // tests and tools: every accepted reply, called from feed()
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning modeacTuning();

} // namespace dect2
