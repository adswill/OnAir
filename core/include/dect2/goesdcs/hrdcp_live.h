// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteosat HRDCP receiver for the engine: IQ in, platform messages out (hrdcp_proto.h for the format).
// The receiver watches the captured band for the 2 s carrier that starts every message, follows each carrier on its own (mixer, CIC filter to about
// 9.6 kHz, root raised cosine matched filter), measures the frequency on the carrier, finds the preamble and marker by correlation (which also gives
// the phase, the level and the symbol timing), then demodulates the offset QPSK with a decision-directed carrier loop and a Gardner timing loop on both
// rails, decodes the convolutional code (soft Viterbi), the randomiser and the Reed-Solomon frames, checks the CRC and keeps the reports and the table
// of platforms. Reports use the DCS report type (goesdcs_tel.h) with the HRDCP fields set; channels follow the Meteosat plan (TD 16 table 11).
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

struct DcsTelemetry;   // goesdcs_tel.h

class HrdcpReceiver {
public:
    HrdcpReceiver();
    ~HrdcpReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);            // not used: the whole captured band is searched
    void setCenterHz(double hz);                // the radio's centre (0 unknown: no channel numbers)
    bool ready() const;
    void reset();                               // the filters and the carriers; the lists stay
    void clearResults();
    void setLogFile(const std::string& path);   // append every message as a CSV line ("" off)
    void setStartUtc(int64_t utcSec);           // tests: the UTC second of the first sample (default: now)
    void setThresholdDb(double db);             // carrier line over the noise (default 12)
    void feed(const cf32* x, size_t n);
    bool telemetry(DcsTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning hrdcpTuning();

} // namespace dect2
