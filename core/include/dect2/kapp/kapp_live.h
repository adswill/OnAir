// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAPP for the engine: IQ in, bursts out. kapp::BurstReceiver finds and decodes the bursts; every data burst goes to a selective-repeat message
// assembler per sender callsign (kapp::ArqReceiver), a message that is complete goes to the packet list of the telemetry. This receiver only listens:
// it never sends the ACK that the protocol asks for (OnAir has no half-duplex turnaround), so a sender that waits for ACKs cannot finish through it;
// a one-way sender repeats the message instead (the transmit option of OnAir does). Family 124, engine standard 126, telemetry 125.
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

struct KappTelemetry;   // kapp_tel.h

class KappLiveReceiver {
public:
    KappLiveReceiver();
    ~KappLiveReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double) {}              // the channel sits on the tuned frequency
    void setCenterHz(double) {}
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(KappTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void clearPackets();                         // empties the packet list and the counts

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning kappTuning();

} // namespace dect2
