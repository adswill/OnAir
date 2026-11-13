// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Z-Wave monitor (ITU-T G.9959 FSK: R1 9.6 kbit/s Manchester, R2 40 kbit/s, R3 100 kbit/s GFSK) on the sub-GHz channels of each region. Receive
// only. Every channel of the region that lies inside the band the radio takes in gets a narrowband front end (mix, low-pass, decimate to 400 ksps);
// an energy detector marks the bursts, each is FM demodulated and tried at the rates of its channel: bit timing from the preamble, start-of-frame
// delimiter, Length, checksum (R1, R2) or CRC-16 (R3). Decoded: home ID, node IDs, frame control, command class and command; S0 and S2
// payloads are named as encrypted and never decoded.
// Engine contract: configure() once the sample rate is known, feed() from the analysis thread, telemetry() and setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct ZwaveTelemetry;   // zwave_tel.h

class ZwaveReceiver {
public:
    ZwaveReceiver();
    ~ZwaveReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);    // the radio's centre frequency; 0 (a file without one): the dial frequency is used
    void setDialHz(double hz);
    void setRegion(const std::string& id);   // "EU", "US", ... ; "" picks by the frequency
    bool ready() const;
    void reset();
    void setToneDetection(bool on);          // chips from tone energies (default on; off: the discriminator only, as in 3.9)
    bool toneDetection() const;
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(ZwaveTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning zwaveTuning();

} // namespace dect2
