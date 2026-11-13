// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NB-IoT cells receiver: the LTE narrowband IoT carriers around you, from what every cell broadcasts in the clear to all devices (3GPP TS
// 36.211 / 212 / 213 / 331): its physical cell id (NPSS / NSSS), operating mode (in-band, guard band, standalone), antenna ports and system frame
// number (MIB-NB), and in SIB1-NB its operator (MCC / MNC), tracking area, cell identity, band and barring. Receive only. Nothing addressed to a
// device is decoded: NPDCCH, paging, random access, every UE-specific channel, the uplink and user data are out of scope, and no subscriber or
// device identity exists in these messages.
//
// The input is taken in blocks of one second. Each block is searched on the 100 kHz raster points of the sample band (and the dial frequency)
// for an NPSS; a carrier that shows one is aligned, its carrier and clock offset measured, the cell id read from the NSSS and the broadcast channels
// decoded. A block is captured only while the previous one is not being searched, so a slow computer just searches less often.
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
#include <vector>

namespace dect2 {

struct NbiotTelemetry;   // nbiot_tel.h

class NbiotReceiver {
public:
    NbiotReceiver();
    ~NbiotReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);   // where the user's (dial) frequency sits relative to 0 Hz in the input
    void setCenterHz(double hz);       // the radio's centre frequency (0: unknown, for files)
    bool ready() const;                // false when the input rate is too low for this mode
    void reset();                      // after a retune: drop the block being captured (the cell table is kept, for band scans)
    void clearCells();                 // the user's Clear button
    void feed(const cf32* x, size_t n);
    bool telemetry(NbiotTelemetry& out, uint64_t lastSeq);
    // The cell table and the event log stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);
    bool busy() const;                 // a block is captured or being searched (tools: feed a file only as fast as it is searched)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning nbiotTuning();

// The LTE bands NB-IoT is deployed in (36.101 NB-IoT operating bands), in the order the band presets list them (Europe first)
const std::vector<int>& nbiotBands();

} // namespace dect2
