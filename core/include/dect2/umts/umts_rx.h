// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// UMTS (3G) cells receiver: every second a block of the channel goes through umtsCellSearch() (primary sync, secondary sync, CPICH) and the cells found are kept in a table.
// Receive only: just the synchronisation channels and the pilot, which carry no identities of subscribers. Operator and cell identity from the BCH (umts_bch.h).
#pragma once
#include "dect2/umts/umts_cell.h"
#include "dect2/engine/mode_tuning.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct UmtsCellRow { int mcc = -1, mnc = -1, mncDigits = 0; int64_t cellIdentity = -1; uint64_t bchBlocks = 0; int psc = -1; int group = -1; double cfoHz = 0, ecIoDb = 0, rscpDbfs = 0; int frameOffset = 0; uint64_t blocks = 0; double lastSeen = 0; };
struct UmtsTelemetry {
    uint64_t seq = 0;
    int state = 0;                   // 0 waiting for samples, 1 scanning
    double cfoHz = 0; bool dataValid = false; float snrDb = 0; uint64_t blocksOk = 0, blocksBad = 0;
    uint64_t scans = 0;
    double lastScanMs = 0;
    std::vector<UmtsCellRow> cells;  // strongest first
};
inline std::string umtsSummary(const UmtsTelemetry& t) { char b[128]; snprintf(b, sizeof b, "UMTS: %zu cell%s", t.cells.size(), t.cells.size() == 1 ? "" : "s"); return b; }

class UmtsReceiver {
public:
    UmtsReceiver();
    ~UmtsReceiver();
    void configure(double inputRateHz);
    bool ready() const;
    void setSignalOffset(double hz);
    void reset();
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(UmtsTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
ModeTuning umtsTuning();
struct SynthConfig; class ModeSynth;
std::unique_ptr<ModeSynth> makeUmtsSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
