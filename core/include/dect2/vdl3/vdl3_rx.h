// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDL Mode 3 receiver: one 25 kHz VHF channel, the D8PSK bursts of ICAO Annex 10 Vol III 6.4.4 (see vdl3_proto.h).
//
// The channel is mixed to 0 Hz, integrated and dumped to 16 samples per symbol (any input rate from 250 kHz up), low-pass filtered (+-13.5 kHz)
// and taken at 8 samples per symbol. Two differential correlators look for the S1 and S2 training sequences; the sign of the correlation
// tells S1 from S1* and S2 from S2* (their phase changes are 180 degrees apart), which holds for carrier offsets up to +-2.6 kHz. A found
// burst gets its carrier offset and phase from the training, a decision-directed phase loop, and differential decoding of the D8PSK
// decisions. S2* bursts come in four lengths (poll response 16, M uplink 32, handoff check 40, 3T M uplink 128 symbols): the length is
// where the burst's power ends, before the next training sequence. Then descrambling, Golay (24,12) and, for V/D bursts, the Reed-Solomon
// (72,62) word: a good word is a data burst, anything else a voice burst (the Mode 3 vocoder is not decoded: no codec).
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread, telemetry() and the setters
// from the interface thread.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_tuning.h"
#include "vdl3_tel.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class Vdl3Receiver {
public:
    Vdl3Receiver();
    ~Vdl3Receiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the channel sits in the input, Hz from 0
    void setCenterHz(double hz);                         // the radio's centre frequency (shown only)
    bool ready() const;
    void reset();                                        // after a retune (the burst list stays: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(Vdl3Telemetry& out, uint64_t lastSeq);
    void clearResults();
    std::string saveResults() const;
    bool loadResults(const std::string& blob);
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning vdl3Tuning();

} // namespace dect2
