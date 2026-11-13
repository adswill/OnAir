// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Drone Remote ID receiver: the Open Drone ID messages (ASTM F3411 / ASD-STAN EN 4709-002) drones broadcast about themselves and their
// operator, heard on both broadcast carriers at once from one 20 Msps capture of the 2.4 GHz band. Receive only: Remote ID is a public
// broadcast by law, meant to be read by anyone; nothing here transmits or answers.
//
// No DSP of its own: the Bluetooth LE advertising demodulator (ble_rx.h) and the 802.11b beacon demodulator (wifi_rx.h) run side by side
// on the same samples, each mixing its channel to 0 Hz, and hand every good packet here (setPacketCallback / setFrameCallback). Packets
// carrying Remote ID (BLE service data 0xFFFA app code 0x0D; Wi-Fi vendor element FA:0B:BC type 0x0D) are decoded with rid_proto.h and
// gathered per broadcast address into a drone table (IDs, UA type, position, height, speed and track, operator position, operator ID).
//
// Frequency plan (ridPlan): with the radio at 2432 MHz (the default) BLE advertising channel 38 (2426) lies 6 MHz below the centre and
// Wi-Fi channel 6 (2437) 5 MHz above it, so both are heard. Tuned elsewhere, the nearest Wi-Fi channel is used and BLE only when an
// advertising channel is within 8 MHz. With a file (no centre known) both demodulators look at 0 Hz.
//
// Burst gate (3.9): the Wi-Fi demodulator (802.11b chain and OFDM receiver at 20 Msps) is handed only the stretches around bursts in its channel.
// A cheap detector looks first: 128-point FFTs over half of every 25.6 us window, the energy in the Wi-Fi channel (the BLE channel and the DC
// spike left out) against its own noise floor (lower quartile of every 16th window over 26 ms). The demodulator gets the windows from 102 us
// before a window with energy (0.8 dB over the floor: an 802.11b frame at -6 dB SNR) to 205 us after one, the bursts joined with noise around
// each. During the first windows, and while the channel is busy more than 60 % of the time, it gets everything as before. BLE is not gated: the
// joins cost packets near its sensitivity.
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

struct RidTelemetry;   // rid_tel.h

struct RidPlan {
    int bleChannel = 0;                  // 37 / 38 / 39, 0 BLE not in the band
    double bleOffsetHz = 0;              // where it lies from the centre
    int wifiChannel = 0;                 // 1 .. 13 (0: no centre known)
    double wifiOffsetHz = 0;
};
RidPlan ridPlan(double centerHz);        // centerHz 0 (a file): both at 0 Hz, channels unknown

class RidReceiver {
public:
    RidReceiver();
    ~RidReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);          // the radio's centre: which channels lie in the band (0: unknown, a file)
    bool ready() const;
    void reset();                         // the demodulators only: the drone table stays
    void clearDrones();
    void feed(const cf32* x, size_t n);
    bool telemetry(RidTelemetry& out, uint64_t lastSeq);
    // The drone table and the message log stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setGate(bool on);                // test knob: the burst gate (on by default); off hands every sample to both demodulators

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning ridTuning();

} // namespace dect2
