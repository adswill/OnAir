// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell towers receiver: the LTE cells around you, from what every cell broadcasts in the clear to all phones (3GPP TS 36.211 / 212 /
// 213 / 331): its physical cell id, bandwidth and antenna ports (MIB), and on SI-RNTI its operator (MCC / MNC), tracking area, cell
// identity, band, power and neighbour lists (SIB1 to SIB5). Receive only. Nothing addressed to a phone is decoded: paging, random access,
// every UE-specific channel, the uplink and user data are out of scope, and no subscriber or device identity exists in these messages.
//
// The input is taken in blocks of 0.2 s. Each block's spectrum is searched for LTE-shaped carriers; every carrier (and the dial
// frequency) is searched for cells (PSS / SSS, carrier and clock offset), their MIB read from the PBCH and, when the whole carrier is in
// the block, the system information decoded. A block is captured only while the previous one is not being searched, so a slow
// computer just searches less often.
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

struct CellTelemetry;   // cell_tel.h

class CellReceiver {
public:
    CellReceiver();
    ~CellReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);   // where the user's (dial) frequency sits relative to 0 Hz in the input
    void setCenterHz(double hz);       // the radio's centre frequency (0: unknown, for files)
    bool ready() const;                // false when the input rate is too low for this mode
    void reset();                      // after a retune: drop the block being captured (the cell table is kept, for band scans)
    void clearCells();                 // the user's Clear button
    void feed(const cf32* x, size_t n);
    bool telemetry(CellTelemetry& out, uint64_t lastSeq);
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

ModeTuning cellTuning();

// ---------------------------------------------------------------- bands and operators (36.101 Table 5.7.3-1)
struct LteBand { int band; double dlLowMhz, dlHighMhz; int nOffsDl; bool tdd; const char* region; };
const std::vector<LteBand>& lteBands();
const LteBand* lteBandInfo(int band);
double lteEarfcnToHz(int earfcn);              // 0 when not in the table
int lteBandOfEarfcn(int earfcn);               // 0 when not in the table
int lteNearestEarfcn(double hz, int preferBand = 0);   // the channel nearest to hz (within 50 kHz), -1 outside every band
std::vector<int> lteBandsOfHz(double hz);      // every band holding hz, most common (European) first
int lteHzToEarfcn(double hz, int preferBand = 0);   // -1 when no band holds it (the 100 kHz raster)
// The band of a cell on hz (#164). Many bands overlap (2110-2155 MHz is band 1, 4, 10, 65 and 66; 758-768 is 14 and 28), so the frequency alone
// does not say. sib1Band is SIB1's freqBandIndicator, 0 when SIB1 is not read yet: 1..63 is the band itself; 64 means "a band above 64,
// the number is in an extension that is not read": the band is then taken from the bands above 64 that hold hz (617-652 MHz: 71).
// confirmed is false when it is a guess from the frequency (the first of the bands that hold it), also lists the other bands that hold hz.
struct LteBandChoice { int band = 0; bool confirmed = false; std::vector<int> also; };
LteBandChoice lteBandFor(double hz, int sib1Band);
// The operator of an MCC-MNC, from a short built-in list of public network codes ("" unknown); the country of the MCC
const char* lteOperatorName(int mcc, int mnc);
const char* lteCountry(int mcc);

} // namespace dect2
