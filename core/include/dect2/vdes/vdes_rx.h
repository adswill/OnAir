// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDES receiver (ITU-R M.2092): ASM on channels 2027 / 2028 (161.950 / 162.000 MHz, 25 kHz either side of AIS 1, GMSK 9.6 kbit/s, HDLC + CRC-16 as AIS; decoded through the AIS
// physical layer, ais_phy.h), and VDE-TER detection on the 100 kHz channels at 157.2 and 161.8 MHz (four 25 kHz sub-channels each): bursts are found
// by a power detector per sub-channel, their bandwidth (25 / 50 / 100 kHz) is the number of sub-channels occupied, and the modulation is classified
// as pi/4-QPSK, 8PSK or 16QAM from the symbol statistics (symbol rate 19.2 kbaud per 25 kHz, root-raised-cosine). VDE-TER content is not decoded.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread, telemetry() and the setters from
// the interface thread. Channels are placed from the dial frequency (setDialHz) and the signal offset.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "vdes_tel.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class VdesReceiver {
public:
    VdesReceiver();
    ~VdesReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);       // where the dial frequency sits relative to 0 Hz of the input
    void setDialHz(double hz);             // the frequency tuned (0: unknown, a file: 161.9 MHz is assumed)
    void setWallClock(bool on);
    void setForwardNmea(bool on);          // AIS-like ASM frames also go out as !AIVDM sentences on the AIS data feed (UDP), when that feed is on (default on)
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(VdesTelemetry& out, uint64_t lastSeq);
    void clearResults();                   // the ASM frame list and the burst log
    std::string saveResults() const;
    bool loadResults(const std::string& blob);
    void setLogCallback(std::function<void(const std::string&)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning vdesTuning();

} // namespace dect2
