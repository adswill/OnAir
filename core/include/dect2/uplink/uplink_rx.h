// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode S uplink receiver: the interrogations ground radars and ACAS aircraft send on 1030 MHz.
//
// Pulses are cut from the envelope (runs over a threshold 10 dB above the noise floor, short dips merged, edges at half amplitude), then
// grouped from each candidate P1:
//   - Mode A/C: P3 8 us (A) or 21 us (C) after P1 at the same level; P2 2 us after P1 is the sidelobe suppression pulse: none or 9 dB below
//     P1 is the main beam, P2 at P1 or above a sidelobe. A P4 2 us after P3 makes it an intermode all-call (1.6 us: A/C/S, 0.8 us: A/C only).
//   - Mode S: P2 2 us after P1 at P1's level, then P6 from 3.5 us, 16.25 or 30.25 us long. The sync phase reversal 1.25 us into P6 is found
//     to a fraction of a sample, every 0.25 us chip after it compared in phase with the one before (DPSK, 4 Mchip/s) with the carrier offset
//     taken out (squared chip products), 56 or 112 bits. A P5 over the sync reversal (sidelobe) hides it: counted, not decoded.
// Needs 8 Msps or more (10 by default). Written to the engine's contract: configure(), feed() from the analysis thread, telemetry() and the
// setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "uplink_msg.h"
#include "uplink_tel.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

// One interrogation, for tests and tools
struct UplinkEvent {
    double sec = 0;            // P1 leading edge
    int kind = 0;              // 0 Mode A/C, 1 intermode all-call, 2 Mode S decoded, 3 Mode S sidelobe (P5), 4 Mode S without sync
    char acMode = 0;           // 'A' or 'C'
    bool sidelobe = false;     // Mode A/C: P2 at or above P1
    int p4 = 0;                // intermode: 1 short P4 (A/C only), 2 long (A/C/S)
    float levelDb = -200, p2Db = -99;   // P1 level dBFS; P2 against P1
    double cfoHz = 0;
    uplink::Uplink msg;        // kind 2
};

class UplinkReceiver {
public:
    UplinkReceiver();
    ~UplinkReceiver();

    void configure(double inputRateHz);
    bool ready() const;
    void reset();                        // after a retune: forget the signal, keep the tables
    void feed(const cf32* x, size_t n);
    void flush();                        // tests: process everything fed so far
    bool telemetry(UplinkTelemetry& out, uint64_t lastSeq);
    // The interrogator and aircraft tables, TCAS list and log stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setEventCallback(std::function<void(const UplinkEvent&)> cb);   // every interrogation, from feed()
    void clear();                        // empty the tables
    void setSilent(bool) {}

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning uplinkTuning();

} // namespace dect2
