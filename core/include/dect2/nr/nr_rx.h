// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 5G NR cells receiver: the NR cells around you, from the SS/PBCH blocks every cell broadcasts in the clear to all phones (3GPP TS 38.211 /
// 212 / 213, 38.104 sync raster): physical cell id (PSS / SSS), SSB index (beam), SS-RSRP / RSRQ / SINR and the MIB (system frame number,
// common subcarrier spacing, k_SSB, DMRS position, CORESET#0 / search space of SIB1, barred). Receive only. Nothing addressed to a phone is
// decoded, nothing is decrypted, and no subscriber or device identity exists in these signals. SIB1 is not decoded.
//
// The input is taken in blocks of 80 ms (four 20 ms SSB periods). Each block searches every sync raster point (GSCN) of the supported
// bands that lies inside the sample band, at the band's SSB subcarrier spacing (and the dial frequency itself). A block is captured only
// while the previous one is not being searched, so a slow computer just searches less often.
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

struct NrTelemetry;   // nr_tel.h

class NrReceiver {
public:
    NrReceiver();
    ~NrReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);   // where the user's (dial) frequency sits relative to 0 Hz in the input
    void setCenterHz(double hz);       // the radio's centre frequency (0: unknown, for files)
    bool ready() const;                // false when the input rate is too low for this mode (15 kHz SSBs need 3.84 Msps, 30 kHz 7.68 Msps)
    void reset();                      // after a retune: drop the block being captured (the cell table is kept, for band scans)
    void clearCells();                 // the user's Clear button
    void feed(const cf32* x, size_t n);
    bool telemetry(NrTelemetry& out, uint64_t lastSeq);
    // The cell table and the event log stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);
    bool busy() const;                 // a block is captured or being searched

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning nrTuning();

} // namespace dect2
