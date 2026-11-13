// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL for the engine: IQ in, speech out to the sound card (honours the shared volume and mute), the last beacon in the telemetry.
// The voice receiver is kaal/kaal_rx.h (it resamples any input rate to 48 kHz itself), the beacon slice receiver is kaal/kaal_beacon.h; both
// get the same IQ. Family 122, engine standard 124, telemetry 123.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread,
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

struct KaalTelemetry;   // kaal_tel.h

class KaalLiveReceiver {
public:
    KaalLiveReceiver();
    ~KaalLiveReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double) {}              // the channel sits on the tuned frequency
    void setCenterHz(double) {}
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(KaalTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setVolume(float v);                     // 0 .. 1
    void setMuted(bool m);
    void setSilent(bool s);                      // decode without opening the sound device (tests)
    void setAudioTap(std::function<void(const int16_t*, size_t)> cb);   // tests: the 16 kHz speech

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning kaalTuning();

// KAVB is kavb/kavb_live.h (engine standard 125), KAPP kapp/kapp_live.h (126), KAFT kaft/kaft_live.h (127).

} // namespace dect2
