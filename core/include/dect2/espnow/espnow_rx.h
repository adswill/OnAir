// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ESP-NOW receiver: the packets ESP8266 / ESP32 boards send each other with Espressif's connectionless protocol, heard on one 2.4 GHz Wi-Fi channel
// from a 20 Msps capture. Receive only: nothing is transmitted, joined or answered.
//
// No PHY of its own: the Wi-Fi demodulator of the Wi-Fi networks mode (wifi_rx.h) runs on the samples, with its 802.11b DSSS chain (1 and 2 Mbit/s,
// long and short preamble) and its OFDM receiver (6 and 12 Mbit/s here; the higher OFDM rates and CCK 5.5 / 11 Mbit/s are counted by it, not read).
// ESP-NOW's default rate is 1 Mbit/s DSSS. Every frame it frames is handed here with its rate and level; vendor-specific action frames from
// Espressif (espnow_proto.h) are kept: a Devices table per sender address (vendor, packet rate, level, last payload) and a Messages log
// (addresses, sequence number, version, payload as hex and text, FCS). Frames whose FCS failed are counted, and listed only when asked for.
// Encrypted unicast frames (CCMP with the peer's LMK) are shown as encrypted, and decrypted only with a key the user entered in Tools > Keys
// (protocol ESP-NOW).
//
// Channel plan (espnowPlan): the Wi-Fi channel 1 - 14 nearest the radio's centre, within 12 MHz; its offset from the centre is mixed out by the
// demodulator. With a file (no centre known) the channel is looked for at 0 Hz.
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

struct EspnowTelemetry;   // espnow_tel.h

struct EspnowPlan {
    int channel = 0;                     // 1 .. 14 (0: no centre known, or not in the 2.4 GHz band)
    double offsetHz = 0;                 // where the channel lies from the centre
};
EspnowPlan espnowPlan(double centerHz);

class EspnowReceiver {
public:
    EspnowReceiver();
    ~EspnowReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);          // the radio's centre: which channel is heard (0: unknown, a file)
    bool ready() const;
    void reset();                         // the demodulator only: the device table stays
    void clearDevices();
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(EspnowTelemetry& out, uint64_t lastSeq);
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setShowBadFcs(bool on);          // also list ESP-NOW frames whose FCS failed (marked; never added to the devices)
    void setReportInterval(double sec);   // seconds of signal between reports (default 0.25; tests)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning espnowTuning();

} // namespace dect2
