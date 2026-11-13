// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Bluetooth Classic (BR/EDR) discovery over a wide capture. Receive only: it never inquires, pages or answers.
//
// Classic Bluetooth hops over 79 channels of 1 MHz (2402 to 2480 MHz) 1600 times a second, so one narrow receiver sees a device about
// 1 time in 79. A wide capture (HackRF at 20 Msps covers 20 channels) sees about a quarter of all packets. Every burst in a channel is cut out,
// shifted to 0 Hz, filtered, resampled to 4 Msps, FM-demodulated and sliced; each of the four sample phases is searched for an access code
// (btc_proto.h): the 64-bit sync word carries the LAP, so any packet of a piconet names its master, and an inquiry response (an FHS packet) also
// gives the full address and the class of device. Devices are kept by LAP. Packets after the access code (data, voice) are neither decoded nor
// followed. GFSK 1 Msym/s, modulation index 0.32 (basic rate); Enhanced Data Rate payloads share the access code and are found the same way.
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

struct BtcTelemetry;   // btc_tel.h

class BtcReceiver {
public:
    BtcReceiver();
    ~BtcReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);      // where the dial frequency sits relative to 0 Hz in the input
    void setCenterHz(double hz);          // the radio's centre: which channels the capture covers (0: unknown, the centre is taken as 2441 MHz)
    bool ready() const;
    void reset();                         // the demodulator only: the device table stays
    void clearDevices();
    void feed(const cf32* x, size_t n);
    bool telemetry(BtcTelemetry& out, uint64_t lastSeq);
    void clearResults();
    void setLogCallback(std::function<void(const std::string&)> cb);
    // Every access code found, on the feeding thread (tests, a mode built on this one): lap, channel MHz, sync word errors, an FHS decoded
    void setHitCallback(std::function<void(uint32_t lap, double mhz, int errors, bool fhs)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning btcTuning();

} // namespace dect2
