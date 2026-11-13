// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CDR sound: the DRA+ decoder (the user-installed add-on, cdr_dra.h) and the sound output of the service the user listens to (used by the receiver, cdr_rx.cpp).
// Everything runs on the receiver thread. The decoder is rebuilt whenever its state could be stale or stuck: a new lock, a lost lock, a manual
// re-sync, another service, and when many audio units go in without any sound coming out; a failed sound device is opened again.
#pragma once
#include "dect2/audio/audioout.h"
#include "dect2/cdr/cdr_dra.h"
#include "dect2/cdr/cdr_mux.h"
#include "dect2/cdr/cdr_tel.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

class CdrAudio {
public:
    explicit CdrAudio(bool headless = false);   // headless: decode and count, open no sound device (tests)
    ~CdrAudio();
    CdrAudio(const CdrAudio&) = delete;
    CdrAudio& operator=(const CdrAudio&) = delete;

    void resetDecoder();    // the next units start a clean decoder (queued sound still plays out)
    void resetAll();        // after a retune: decoder and the service switch memory
    void flush();           // drop the queued sound
    // The audio units of one multiplex sub-frame of service `id`; `want` is the service the user picked (0: none). Counts into `tel`.
    void play(int want, uint16_t id, const std::vector<cdr::AudioUnit>& units, float volume, bool muted, CdrTelemetry& tel);

private:
    void* dra_ = nullptr;
    const cdr::DraApi* api_ = nullptr;      // the add-on's functions; null until the library is found
    int rate_ = 0;
    int playingId_ = -1, lastListen_ = -1;
    bool headless_ = false;
    int unitsSincePcm_ = 0, deviceTries_ = 0;
    std::unique_ptr<AudioOut> out_;
};

} // namespace dect2
