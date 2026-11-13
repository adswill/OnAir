// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NB-IoT receiver telemetry, shared by the receiver and the UI.
// Only what an NB-IoT cell broadcasts to every device is here: its identity, its operator, its operating mode and its level.
// The first members of NbiotTelemetry are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One NB-IoT cell, keyed by its frequency and physical cell id
struct NbCell {
    double freqHz = 0;            // the carrier's 100 kHz raster frequency (the offset from the dial when the radio's frequency is not known)
    double offsetKhz = 0;         // where the carrier sits from the raster: measured, or from the MIB-NB when it says
    int earfcn = -1;              // the raster's LTE channel number, -1 unknown
    int band = 0;                 // from SIB1-NB when it was read, else from the frequency
    int pci = 0;
    bool hasMib = false;          // the NPBCH decoded at least once (else the cell is known from NPSS / NSSS only)
    bool hasSib1 = false;
    int mode = -1;                // 0 in-band same PCI, 1 in-band other PCI, 2 guard band, 3 standalone (MIB-NB)
    int ports = 0;                // NRS antenna ports
    int sfn = 0, hsfnLsb = 0, sched = 0, valueTag = 0;
    bool ab = false;              // access barring enabled
    std::vector<std::string> plmns;   // "246-01", the first is the primary
    int mcc = -1, mnc = -1, mncDigits = 2;
    std::string op;               // operator name from the MCC-MNC table ("" unknown)
    uint32_t tac = 0;             // tracking area code
    uint32_t cellId = 0;          // 28 bits
    uint32_t enb = 0;             // eNB id (cellId >> 8)
    int sector = 0;
    bool barred = false, reservedOp = false;
    int qRxLevMinDbm = 0, pMaxDbm = -99;
    // measurements (dBFS: relative to the radio's full scale, not calibrated)
    float levelDb = -200, snrDb = -99, npssScore = 0;
    double cfoHz = 0, clockPpm = 0;
    int64_t firstWall = 0, lastWall = 0;
    double lastSec = 0;
    uint64_t seen = 0;            // blocks in which it was found
    uint64_t sib1Msgs = 0;        // SIB1-NB messages decoded
};

struct NbEvent { int64_t wallTime = 0; double sec = 0; std::string text; };

struct NbiotTelemetry {
    uint64_t seq = 0;
    int state = 0;                // 0 no input yet, 1 searching, 2 a cell was found in the last 10 s
    double cfoHz = 0;             // of the strongest cell
    float snrDb = 0;              // SNR of the strongest cell
    float levelDb = -200;         // input power, dBFS
    bool dataValid = false;       // a cell was found in the last 10 s
    uint64_t blocksOk = 0;        // SIB1-NB messages decoded
    uint64_t blocksBad = 0;       // SIB1-NB tries whose transport block failed its CRC
    double timeSec = 0;
    double inputRate = 0;
    double centerHz = 0;          // the radio's centre (0: unknown)
    double dialOffsetHz = 0;
    uint64_t blocks = 0;          // capture blocks searched
    uint64_t retunes = 0;         // centre changes seen (scan progress)
    double lastBlockCenterHz = 0; // the centre of the last block searched
    std::vector<NbCell> cells;    // most recently seen first
    std::vector<NbEvent> events;  // newest first, at most kNbLogMax
};

constexpr size_t kNbLogMax = 300;
constexpr size_t kNbCellMax = 400;

inline const char* nbiotModeName(int m) {
    switch (m) {
    case 0: return "in-band (same PCI)";
    case 1: return "in-band (other PCI)";
    case 2: return "guard band";
    case 3: return "standalone";
    default: return "?";
    }
}

inline std::string nbiotSummary(const NbiotTelemetry& t) {
    if (t.state == 0) return "waiting for samples";
    size_t named = 0;
    for (const auto& c : t.cells) if (c.hasSib1 && !c.plmns.empty()) named++;
    char b[160];
    snprintf(b, sizeof b, "%zu NB-IoT cell%s, %zu identified", t.cells.size(), t.cells.size() == 1 ? "" : "s", named);
    return b;
}

} // namespace dect2
