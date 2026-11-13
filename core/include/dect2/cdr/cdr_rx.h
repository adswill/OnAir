// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CDR (China Digital Radio, GY/T 268): OFDM digital radio in the FM band.
// Skeleton: counts the samples, measures the input level and reports four times a second of signal (state 0: no decoder yet); the
// mode's worker replaces the body. Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis
// thread (never blocks), telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "cdr_tel.h"
#include "dect2/common/ring.h"
#include "dect2/tv/es_save.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class CdrReceiver {
public:
    CdrReceiver();
    ~CdrReceiver();

    void configure(double inputRateHz);
    bool ready() const;                                  // false when the input rate is too low for this mode
    void resync();                                       // keep the radio running: forget the lock and the half-decoded frames, search the beacon again
    void reset();                                        // after a retune: forget the signal and the decoded state
    void feed(const cf32* x, size_t n);
    bool telemetry(CdrTelemetry& out, uint64_t lastSeq);
    // Write the raw DRA+ audio units of one service to a file (the units as received, one after the other: a DRA elementary stream FFmpeg can read).
    bool saveAudioStream(uint16_t serviceId, const std::string& path);
    void stopSaveAudioStream();
    bool savingAudioStream() const;
    uint64_t savedAudioBytes() const;
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread
    // Play this service's DRA+ audio (0: silence). The interface calls it for the service the user picked.
    void setListen(int serviceId);      // 0: none; -1: the first audio service (the phone app)
    void setVolume(float v);
    void setMuted(bool m);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning cdrTuning();

} // namespace dect2
