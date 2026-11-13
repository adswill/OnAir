// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sigfox uplink mode (its own mode entry; the ISM sensors mode also listens for Sigfox on its bands). The narrow-carrier finder and D-BPSK
// demodulator of ism_sigfox.h on +-96 kHz around the dial, with the frame layer of sigfox_proto.h: device ID, message counter, payload (hex),
// authentication code (shown, never checked: it needs the device key), CRC (checked), the three repetitions merged. Receive only.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread, telemetry() and the setters
// from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct SigfoxModeTelemetry;

class SigfoxReceiver {
public:
    SigfoxReceiver();
    ~SigfoxReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);   // where the dial sits in the input
    void setDialHz(double hz);         // the dial's absolute frequency (0: unknown, a file)
    bool ready() const;
    void reset();
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(SigfoxModeTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning sigfoxTuning();

}  // namespace dect2
