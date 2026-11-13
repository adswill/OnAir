// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// EBICab / Eurobalise up-link receiver ("needs a user test": no real recording was available). The up-link is a magnetic field around 4.234 MHz
// that a passive balise sends while the train's tele-powering field (27.095 MHz) is over it: FSK with 3.951 MHz for 0 and 4.516 MHz for 1,
// 564.48 kbit/s (UNISIG SUBSET-036). The radio is tuned about 1 MHz above 4.234 MHz (the signal stays clear of the DC spike), the channel is mixed down,
// low-passed and decimated to about 3.3 Msps, FM demodulated, sliced with a clock loop, and the bits go to the receiver of SUBSET-036 4.3.4.1
// (ebicab_proto.h). Bursts are found from the signal power. The telegram formats of EBICab 700 (32 bits) and EBICab 900 (255 bits) are not published:
// bursts that give no Eurobalise telegram are listed with their length and first bits, nothing more.
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

struct EbicabTelemetry;   // ebicab_tel.h

class EbicabReceiver {
public:
    EbicabReceiver();
    ~EbicabReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);     // where 4.234 MHz sits in the samples
    bool ready() const;
    void reset();
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(EbicabTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void flush();                        // tests: close the burst that is open
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning ebicabTuning();

} // namespace dect2
