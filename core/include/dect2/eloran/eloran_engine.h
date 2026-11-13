// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// eLoran / Loran-C in the engine: the detector of eloran_rx.h behind the engine's interface (configure, offset, feed, telemetry).
// Engine stdMode 128, active standard 127 (family 126, "eLoran / Loran-C"). Receive only; nothing is transmitted.
// Loran-C: GRI, chain, master and secondaries with their arrival times. The eLoran data channel (9th pulse, Eurofix) is not decoded: the Reed-Solomon constants and message
// layouts of the Loran Data Channel standard are not public text (see eloran_rx.h).
#pragma once
#include "dect2/eloran/eloran_rx.h"
#include "dect2/engine/mode_tuning.h"
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct ElLoranTelemetry {
    uint64_t seq = 0;
    int state = 0;                  // 0 no input, 1 searching (no GRI), 2 GRI found, 3 locked (same GRI twice, master seen)
    double cfoHz = 0;               // not measured
    float snrDb = 0;                // detection margin of the strongest station, dB
    float levelDb = -200;           // level of the strongest station, dBFS
    bool dataValid = false;         // a GRI is found
    uint64_t blocksOk = 0;          // analyses that found a GRI
    uint64_t blocksBad = 0;         // analyses that found none
    std::string chain;              // the chain that uses this GRI, empty when unknown
    int griUs = 0;                  // group repetition interval, microseconds (9960 is written GRI 9960 = 99600 us)
    double zScore = 0;
    bool locked = false;
    bool masterFound = false;
    std::vector<loran::Station> stations;
    std::vector<float> pulseEnv;    // averaged pulse envelope
    double envStartUs = 0, envStepUs = 0;
    double inputRate = 0;
    double centerHz = 0;
};

class ElLoranReceiver {
public:
    ElLoranReceiver();
    ~ElLoranReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);     // where the 100 kHz carrier sits relative to 0 Hz in the input (about -tuneOffsetHz)
    void setCenterHz(double hz);
    bool ready() const;
    void reset();
    void feed(const std::complex<float>* x, size_t n);
    bool telemetry(ElLoranTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning elLoranTuning();

// One line for the top bar and the receiver tab
std::string elLoranSummary(const ElLoranTelemetry& t);

}  // namespace dect2
