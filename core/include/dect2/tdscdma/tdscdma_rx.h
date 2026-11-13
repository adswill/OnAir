// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TD-SCDMA cells receiver: every 80 ms block (16 sub-frames) of the 1.6 MHz channel goes through tdscdma::search() whenever the previous one
// is done, and the cells heard are kept in a table by SYNC-DL code and cell parameter. Receive only (tdscdma.h).
#pragma once
#include "dect2/tdscdma/tdscdma.h"
#include "dect2/engine/mode_tuning.h"
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct TdscdmaCellRow {
    int syncDl = -1, cellParam = -1;
    bool paramSure = false;      // the initial parameter, not only its pair
    double levelDbfs = -200, snrDb = 0, cfoHz = 0;
    float midScore = 0;
    uint64_t blocks = 0, quadS1 = 0, quadS2 = 0;
    double lastSeen = 0;
};
struct TdscdmaTelemetry {
    uint64_t seq = 0;
    int state = 0;
    double cfoHz = 0; bool dataValid = false; float snrDb = 0; uint64_t blocksOk = 0, blocksBad = 0;
    uint64_t scans = 0;
    double lastScanMs = 0;
    std::vector<TdscdmaCellRow> cells;   // strongest first
};
inline std::string tdscdmaSummary(const TdscdmaTelemetry& t) { char b[96]; snprintf(b, sizeof b, "TD-SCDMA: %zu cell%s", t.cells.size(), t.cells.size() == 1 ? "" : "s"); return b; }

class TdscdmaReceiver {
public:
    TdscdmaReceiver();
    ~TdscdmaReceiver();
    void configure(double inputRateHz);
    bool ready() const;
    void setSignalOffset(double hz);
    void reset();
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(TdscdmaTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    bool busy() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
ModeTuning tdscdmaTuning();
struct SynthConfig; class ModeSynth;
std::unique_ptr<ModeSynth> makeTdscdmaSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
