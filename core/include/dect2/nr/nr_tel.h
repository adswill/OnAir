// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 5G NR cells receiver telemetry, shared by the receiver and the UI.
// Only what an NR cell broadcasts to every phone is here: its physical cell id, its SS/PBCH blocks (beams) and its MIB.
// The first members of NrTelemetry are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct NrBeamSeen { int ssbIdx = 0; float rsrpDb = -200; };

// One NR cell, keyed by its SSB frequency and physical cell id
struct NrCell {
    double freqHz = 0;            // SS_REF (the GSCN frequency when the radio's frequency is known, else the offset from the dial)
    int gscn = -1;                // -1: unknown (a file, or off the sync raster)
    int band = 0;                 // from the frequency and the subcarrier spacing
    int pci = 0, nid1 = 0, nid2 = 0;
    int scsKhz = 30;              // SSB subcarrier spacing
    // MIB
    bool hasMib = false;          // the PBCH decoded at least once (else the cell is known from PSS / SSS only)
    int sfn = 0;                  // the last system frame number read
    bool hrf = false;
    int scsCommonKhz = 0;         // subCarrierSpacingCommon (15 or 30 in FR1)
    int kSsb = 0;                 // ssb-SubcarrierOffset with its MSB (0..31)
    int dmrsTypeAPos = 2;
    int coreset0 = 0, ss0 = 0;    // pdcch-ConfigSIB1
    bool barred = false, intraFreqResel = true;
    int lmax = 8;
    int ssbIdx = 0;               // the strongest SSB's index
    bool sib1Scheduled = false;   // k_SSB <= 23: a CORESET#0 for SIB1 exists
    int cs0Rbs = 0, cs0Syms = 0, cs0OffRb = 0;   // CORESET#0 from 38.213 Tables 13-1 to 13-4 (0: unknown / reserved)
    std::vector<NrBeamSeen> beams;                // every SSB index heard, with its RSRP
    // measurements (dBFS per resource element: relative to the radio's full scale, not calibrated)
    float rsrpDb = -200, rsrqDb = -99, sinrDb = -99, pssDb = 0;
    double cfoHz = 0;
    int64_t firstWall = 0, lastWall = 0;
    double lastSec = 0;
    uint64_t seen = 0;            // blocks in which it was found
    uint64_t mibs = 0;            // PBCH decodes
};

struct NrEvent { int64_t wallTime = 0; double sec = 0; std::string text; };

struct NrTelemetry {
    uint64_t seq = 0;
    int state = 0;                // 0 no input yet, 1 searching, 2 a cell was found in the last 10 s
    double cfoHz = 0;             // of the strongest cell
    float snrDb = 0;              // SS-SINR of the strongest cell
    float levelDb = -200;         // input power, dBFS
    bool dataValid = false;       // a cell was found in the last 10 s
    uint64_t blocksOk = 0;        // PBCH blocks decoded (MIB)
    uint64_t blocksBad = 0;       // PBCH blocks that failed their CRC
    double timeSec = 0;
    double inputRate = 0;
    double centerHz = 0;          // the radio's centre (0: unknown)
    double dialOffsetHz = 0;
    uint64_t blocks = 0;          // capture blocks searched
    uint64_t retunes = 0;         // centre changes seen (scan progress)
    double lastBlockCenterHz = 0; // the centre of the last block searched
    int lastGscnLo = -1, lastGscnHi = -1, lastCandidates = 0;   // the sync raster points searched in the last block
    std::vector<NrCell> cells;    // most recently seen first
    std::vector<NrEvent> events;  // newest first, at most kNrLogMax
};

constexpr size_t kNrLogMax = 300;
constexpr size_t kNrCellMax = 300;

inline std::string nrSummary(const NrTelemetry& t) {
    if (t.state == 0) return "waiting for samples";
    size_t mib = 0, beams = 0;
    for (const auto& c : t.cells) { if (c.hasMib) mib++; beams += c.beams.size(); }
    char b[160];
    snprintf(b, sizeof b, "%zu NR cell%s, %zu with MIB, %zu beam%s", t.cells.size(), t.cells.size() == 1 ? "" : "s", mib, beams, beams == 1 ? "" : "s");
    return b;
}

} // namespace dect2
