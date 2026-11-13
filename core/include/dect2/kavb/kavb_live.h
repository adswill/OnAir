// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAVB for the engine: IQ in, container packets out. The radio-rate samples are mixed by a coarse carrier correction, resampled to 4 samples per symbol of the
// symbol rate that is set (profile A 700, B 1050, C and D 1400 ksym/s: the receiver listens to one symbol rate at a time, and C and D share one), and given to
// kavb::Receiver (frames), then to kavb::Depacketiser (FEC groups and packets). No AV1 or Opus decoder is built in: the telemetry lists the packets and counts
// their payload bytes, and the payload can be written to a file.
//
// The coarse carrier search: while no frame header passes its CRC, the mixer steps through offset hypotheses of 0, +-4, +-8, +-12% of the symbol rate (0.1 s each, 0.7 s for the
// round), so that one of them leaves the carrier within what kavb::Receiver takes by itself (+-2.5% of the symbol rate). The search stops where a frame is found and starts again
// around that offset after 0.5 s without one. A signal must therefore last longer than a round to be found outside the first hypothesis' range.
// Family 123, engine standard 125, telemetry slot 133.
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

struct KavbTelemetry;   // kavb_tel.h

class KavbLiveReceiver {
public:
    KavbLiveReceiver();
    ~KavbLiveReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double) {}              // the channel sits on the tuned frequency (the coarse search finds the residual)
    void setCenterHz(double) {}
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(KavbTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    // The symbol rate to listen to: 0 profile A (700 ksym/s), 1 B (1050), 2 C or 3 D (1400). Takes effect with the next samples.
    void setListenProfile(int profile);
    int listenProfile() const;
    // Appends the payload bytes of the received packets (every type but fill and the station ID text) to this file, in arrival order; empty stops writing.
    void setOutputFile(const std::string& path);
    void clearPackets();                         // empties the packet list and the counts

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning kavbTuning();

} // namespace dect2
