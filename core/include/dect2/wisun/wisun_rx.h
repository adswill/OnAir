// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-SUN FAN monitor: the frames smart meters, street lights and their border routers send on the 902 - 928 MHz band (IEEE 802.15.4g
// SUN FSK). Receive only: it never transmits, joins or answers a network; what is encrypted (PAN configuration and data frames: the keys come
// from an EAP-TLS exchange) is shown as encrypted, not decoded; only what is sent in the clear is decoded.
//
// The nodes hop over the channels on their own schedules, so the receiver does not follow anyone: it takes in as much of the band as the
// sample rate allows (all of 915 - 928 MHz at 16 Msps or more) and listens on every channel at once.
// A polyphase filter bank splits the input into bins of about 300 kHz (rate / M, M a power of two), each at twice the bin spacing. An energy
// detector per bin marks bursts (with some samples before the rise). Every burst is handed to the channels of the 200 kHz grid whose
// nearest bin it is: each mixes the burst to 0 Hz, low-pass filters it, takes the frequency (FM discriminator) and, for each bit rate the
// band plan's channel plans carry there (50, 100, 150 kbit/s), looks for the start-of-frame delimiter after the 0101 preamble at several sample
// phases (one bit error allowed). The frame then follows with the carrier offset from the preamble and a zero-crossing clock tracker; the PHR
// gives the length, whitening and FCS type; the PSDU is dewhitened (PN9) and kept when the FCS passes, or when flipping some of its least
// reliable bits makes it pass (the CRC-32 is linear: the flips are found from the syndrome). The bursts are demodulated on a thread of the
// receiver's own, beside the feed thread, which keeps the filter bank and the tables.
// Decoded: the MAC header and the Wi-SUN IEs (wisun_proto.h); a table of networks, nodes and links, a frame log and a hop chart.
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

struct WisunTelemetry;   // wisun_tel.h

class WisunReceiver {
public:
    WisunReceiver();
    ~WisunReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    // The radio's centre frequency: where every channel lies in the input. 0 (a file without a frequency): the dial frequency is used.
    void setCenterHz(double hz);
    void setDialHz(double hz);
    // The band plan (wisun::bandPlans(): "ANZ", "NA", "EU863", "JP", ...) whose channels and FSK modes the receiver listens with; "" picks
    // by the frequency (wisun::bandPlanFor)
    void setRegion(const std::string& r);
    bool ready() const;
    void reset();                         // the demodulator only: the tables stay
    void clearResults();
    void feed(const cf32* x, size_t n);
    // Tools and tests: waits for the decoder thread (bursts are decoded beside the feed thread) and publishes a report with everything it
    // decoded. Never from the feed path of a live receiver.
    void flush();
    // Tools and tests that feed faster than real time: feed() waits for the decoder instead of dropping bursts when it falls behind
    void setOffline(bool on);
    bool telemetry(WisunTelemetry& out, uint64_t lastSeq);
    // The tables and the frame log stay through reset(), configure() and retunes: only clearResults() empties them
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning wisunTuning();

} // namespace dect2
