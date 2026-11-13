// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DJI drone receiver: detection by signal shape (OcuSync-like, Lightbridge-like, unknown wideband OFDM) and DJI DroneID decoding. Receive only.
//
// The radio must give at least 16 Msps (20 Msps for a 20 MHz-wide OcuSync signal to fit): the bursts are found from the signal's power in 10 us steps,
// each is measured (occupied width and centre from the spectrum, length, level, cyclic-prefix correlation for OFDM-ness) and tracked with the others of the
// same frequency and width: the period and duty cycle of the bursts and the shape decide the class. A burst that is 560 - 700 us long and 7 - 11 MHz wide
// is also tried as DroneID (dji_proto.h: Zadoff-Chu symbols, turbo decoding, CRC-24); a packet that passes gives the drone's serial number, position, home
// point and pilot position as sent in the clear. Nothing else is decoded: the video link is encrypted and proprietary.
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

struct DjiTelemetry;   // dji_tel.h

class DjiReceiver {
public:
    DjiReceiver();
    ~DjiReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);                // the radio's RF centre (0: unknown, a file): frequencies and bands in the report
    void setWallClock(bool on);                 // stamp with the computer's clock (a radio; off for a file)
    void setDecodeDroneId(bool on);             // try DroneID on suitable bursts (default on)
    bool ready() const;
    void reset();                               // the detector; tracks, bursts and drones stay
    void clearDrones();
    void feed(const cf32* x, size_t n);
    bool telemetry(DjiTelemetry& out, uint64_t lastSeq);
    void clearResults();                        // drones, tracks and the burst log
    std::string saveResults() const;            // the drones heard (results_io.h)
    bool loadResults(const std::string& blob);  // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning djiTuning();

} // namespace dect2
