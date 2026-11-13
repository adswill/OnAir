// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FLARM receiver: the glider / light-aircraft collision-avoidance broadcast on 868.2 and 868.4 MHz (Europe), 2-FSK 100 kchip/s Manchester coded,
// old protocol (V6) decoded with the public XXTEA key scheme, the newer protocol only counted. Receive only, as the Open Glider Network does:
// positions are what the aircraft broadcast openly; the no-track and stealth flags are honoured (such aircraft are counted, never kept).
// Both channels are taken from one radio tuned to 868.3 MHz (sample rate >= 1 Msps, works on an RTL-SDR).
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

struct FlarmTelemetry;   // flarm_tel.h

class FlarmReceiver {
public:
    FlarmReceiver();
    ~FlarmReceiver();
    static inline bool chaseOn = true;
    static inline bool toneDetect = true;   // 3.9: chips by the energy at +-50 kHz instead of the FM discriminator (switch for comparisons)   // Chase decoding of CRC-failed packets (3.9; switch for comparisons)
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setReference(double latDeg, double lonDeg);       // the receiver's position: needed to complete the positions, and for distance / bearing
    void clearReference();
    void setWallClock(bool on);                            // the packets' key time is the computer's clock (a radio; default on)
    void setUnixTime(double unixSecAtStart);               // wall clock off (files, tests): the time of the first sample
    void setExpiry(double seconds);                        // an aircraft silent this long leaves the table (default 60)
    bool ready() const;
    void reset();                                          // the demodulators; the aircraft stay
    void feed(const cf32* x, size_t n);
    bool telemetry(FlarmTelemetry& out, uint64_t lastSeq);
    void clearResults();
    std::string saveResults() const;                       // the aircraft table only (never anything flagged private)
    bool loadResults(const std::string& blob);
    void setLogCallback(std::function<void(const std::string&)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning flarmTuning();

} // namespace dect2
