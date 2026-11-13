// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HRPT / AHRPT receiver: the high-resolution weather satellite downlinks near 1.7 GHz. A dish is needed (1 m or more, or a good grid) with
// an LNA at the feed, and a fast radio: 4 Msps at the least, 6 - 10 Msps better (HackRF, Airspy, SDRplay, Pluto; not an RTL-SDR).
//
// MetOp AHRPT (MetOp-B and -C, 1701.3 MHz): QPSK at 2.33 Msym/s, CCSDS K = 7 r = 1/2 code, CADUs with Reed-Solomon (255,223) x4; the
// AVHRR/3 lines (APID 103 / 104) make the picture. FY-3 AHRPT (about 2.8 Msym/s): the same chain, frames and virtual channels counted
// only (its instrument packets are not decoded). NOAA HRPT (BPSK, Manchester, 665.4 kbit/s on 1698 / 1702.5 / 1707 MHz): NOAA-15, 18
// and 19 are retired (2025), so this path is only for archived recordings; low priority in the automatic search.
//
// Demodulator: mixed to 0 Hz (and decimated when the rate allows), coarse carrier (raised cosine fitted to the spectrum, or the squared
// signal's line for BPSK), root raised cosine matched filter, Gardner timing with cubic interpolation, a Costas loop, soft symbols; the
// phase ambiguity from the encoded sync word, Viterbi, frames. Written to the engine's contract: configure() once the sample rate is
// known, feed() from the analysis thread (never blocks), telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct HrptTelemetry;   // hrpt_tel.h

class HrptReceiver {
public:
    HrptReceiver();
    ~HrptReceiver();

    void configure(double inputRateHz);   // the picture stays
    void setSignalOffset(double hz);
    void setVariant(int v);               // 0 automatic, 1 MetOp AHRPT, 2 FY-3 AHRPT, 3 NOAA HRPT
    int variant() const;
    bool ready() const;
    void reset();                         // the demodulator only: the picture stays
    void clearImage();                    // start a new picture
    void feed(const cf32* x, size_t n);
    bool telemetry(HrptTelemetry& out, uint64_t lastSeq);
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning hrptTuning();

struct HrptPreset { const char* label; double mhz; int variant; };
const HrptPreset* hrptPresets(int& count);   // 1701.300 (default), FY-3, then the retired NOAA frequencies
double hrptSymbolRate(int variant);

} // namespace dect2
