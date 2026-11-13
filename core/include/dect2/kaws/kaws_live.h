// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS for the engine: IQ in, decoded chat messages and beacons out. The radio's IQ (the dial frequency at 0 Hz after the signal offset, upper sideband:
// audio frequency = offset from the dial) is brought to 24 kHz complex, then to 12 kHz real audio (the sound-card rate of the mode). The slot grid of
// every slotted profile (P0 120 s, P1 and P2 60 s, P3 30 s, P4 and P5 15 s) comes from the computer clock, which must be UTC-accurate (NTP): a burst
// is looked for from 1.5 s before the slot to 4.5 s after its start. When a slot has passed the audio of its window goes to a worker thread that runs the
// Rust receiver (preamble search, ARQ-less decode of the blocks); the first block gives the length of the message, a message that is complete goes to the
// lists of the telemetry. This receiver only listens: it sends no acknowledgement, so a sender that waits for acks needs more than one slot of the
// same message (the OnAir transmitter repeats the message instead). Voice (P6, P7) is not received. Family 133, engine standard 135, telemetry 134.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks, the search runs on a worker),
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

struct KawsTelemetry;   // kaws_tel.h

class KawsLiveReceiver {
public:
    KawsLiveReceiver();
    ~KawsLiveReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);             // where the dial frequency sits relative to 0 Hz in the input
    void setCenterHz(double) {}
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(KawsTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void clearLists();                           // empties the chat and beacon lists and the counts

    // UTC seconds since the epoch. The default is the computer's clock; a test gives its own.
    void setClock(std::function<double()> utcNow);
    // Waits until the slot searches that are queued have finished (tests, command line).
    void flush();

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning kawsTuning();

} // namespace dect2
