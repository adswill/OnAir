// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ANT+ sensor monitor: the broadcasts of heart rate straps, bike sensors, power meters, footpods and other ANT+ devices on 2457 MHz
// (and any ANT network on its own channel), the same frames a watch or bike computer listens to. Receive only: it never transmits,
// pairs or acknowledges, and shows only what every device broadcasts in the clear.
//
// 1 Mbit/s GFSK, about 160 kHz deviation. The channel is mixed to 0 Hz, low-pass filtered and resampled to 4 Msps (4 samples per bit),
// FM-demodulated and smoothed over a bit. Each sample phase slices bits against a running mean and looks for the preamble and the ANT+
// network marker (0xAA / 0x55 then 0xA6C5, one bit error allowed); for other ANT networks it looks for the preamble followed by a frame
// whose length byte and CRC agree. The hit's 24 known bits give the eye (the decision level is the carrier offset, the spread the SNR),
// then the 17 bytes are sliced with that level and kept when the CRC-16 passes (ant_proto.h). Sensors are kept by device number and type.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "ant_tel.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class AntReceiver {
public:
    AntReceiver();
    ~AntReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);          // the radio's centre (0: a file): shown as the channel
    bool ready() const;
    void reset();                         // the demodulator only: the sensor table stays
    void feed(const cf32* x, size_t n);
    bool telemetry(AntTelemetry& out, uint64_t lastSeq);
    // The sensor table and the frame log stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setAnyNetwork(bool on);          // also frames of other ANT networks (their marker is not 0xA6C5); default on
    void setWheelMm(int mm);              // wheel circumference for the speed profiles (default 2096 mm)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning antTuning();

} // namespace dect2
