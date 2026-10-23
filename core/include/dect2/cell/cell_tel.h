// Cell towers receiver telemetry, shared by the receiver and the UI.
// Only what an LTE cell broadcasts to every phone is here: its identity, its operator, its radio set-up and its neighbours.
// The first members of CellTelemetry are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct CellNeigh { int pci = 0, qOffsetDb = 0; };
struct CellInterFreq { int earfcn = 0, bwRb = 0, priority = -1, qRxLevMinDbm = 0, threshHighDb = 0, threshLowDb = 0; std::vector<CellNeigh> cells; };
struct CellSched { int periodRf = 0; std::vector<int> sibs; };

// One LTE cell, keyed by its frequency and physical cell id
struct CellTower {
    // found by the synchronisation signals and the MIB
    double freqHz = 0;            // the carrier (the raster frequency when the radio's frequency is known, else the offset from the dial)
    int earfcn = -1;              // -1: unknown (a file, or a frequency outside the band table)
    int band = 0;                 // from SIB1 when it was read, else the first band that holds the frequency (bandConfirmed false)
    bool bandConfirmed = false;   // SIB1's freqBandIndicator (or the only band above 64 that holds the frequency) says so
    std::vector<int> bandsAlso;   // the other bands that hold the frequency (2125 MHz: band 4 is also 1, 10, 65, 66)
    bool mfbi = false;            // bandsAlso is the cell's own multiBandInfoList from SIB1 (multiple frequency band indicator), not a guess
    int pci = 0;
    bool tdd = false, extCp = false;
    int nRb = 0, ports = 0, phichDur = 0, phichRes = 0;   // MIB: bandwidth in resource blocks, antenna ports, PHICH duration / Ng index
    int sfn = 0;                  // the last system frame number read
    // SIB1
    bool hasMib = false;          // the PBCH decoded at least once (else the cell is known from PSS / SSS only)
    bool hasSib1 = false;
    std::vector<std::string> plmns;   // "246-01", the first is the primary
    int mcc = -1, mnc = -1, mncDigits = 2;
    std::string op;               // operator name from the MCC-MNC table ("" unknown)
    uint32_t tac = 0;             // tracking area code
    uint32_t cellId = 0;          // 28 bits
    uint32_t enb = 0;             // eNB id (cellId >> 8)
    int sector = 0;               // cellId & 255
    bool barred = false, reservedOp = false;
    int qRxLevMinDbm = 0, pMaxDbm = -99;
    std::vector<CellSched> sched;
    int siWindowMs = 0, valueTag = -1;
    // SIB2
    bool hasSib2 = false;
    int refSignalPowerDbm = 0, pb = 0, ulEarfcn = -1, ulBwRb = 0, raPreambles = 0, modPeriodCoeff = 0, pagingCycleRf = 0;
    bool acBarring = false;
    int t300 = 0, t310 = 0, n310 = 0, timeAlignMs = 0;
    // SIB3
    bool hasSib3 = false;
    int qHystDb = 0, prio = 0, sIntraDb = -1, sNonIntraDb = -1, threshServingLowDb = 0, tReselSec = 0;
    // SIB4 / SIB5
    bool hasSib4 = false, hasSib5 = false;
    std::vector<CellNeigh> intra;           // SIB4 intra-frequency neighbours
    std::vector<CellInterFreq> inter;       // SIB5 other frequencies
    std::vector<int> sibsSeen;              // SIB numbers decoded (1 = SIB1)
    // measurements (dBFS: relative to the radio's full scale, not calibrated)
    float rsrpDb = -200, rsrqDb = -99, sinrDb = -99, pssDb = 0;
    double cfoHz = 0, clockPpm = 0;
    int64_t firstWall = 0, lastWall = 0;
    double lastSec = 0;
    uint64_t seen = 0;            // blocks in which it was found
    uint64_t siMsgs = 0;          // system information messages decoded
};

struct CellEvent { int64_t wallTime = 0; double sec = 0; std::string text; };
struct CellCarrierSeen { double freqHz = 0; double widthHz = 0; float levelDb = 0; };   // an LTE-like block in the spectrum

struct CellTelemetry {
    uint64_t seq = 0;
    int state = 0;                // 0 no input yet, 1 searching, 2 a cell was found in the last 10 s
    double cfoHz = 0;             // of the strongest cell
    float snrDb = 0;              // SINR of the strongest cell
    float levelDb = -200;         // input power, dBFS
    bool dataValid = false;       // a cell was found in the last 10 s
    uint64_t blocksOk = 0;        // system information messages decoded
    uint64_t blocksBad = 0;       // SI-RNTI allocations whose transport block failed its CRC
    double timeSec = 0;
    double inputRate = 0;
    double centerHz = 0;          // the radio's centre (0: unknown)
    double dialOffsetHz = 0;
    uint64_t blocks = 0;          // capture blocks searched
    uint64_t retunes = 0;         // centre changes seen (scan progress)
    double lastBlockCenterHz = 0; // the centre of the last block searched
    std::vector<CellCarrierSeen> carriers;   // in the last block
    std::vector<CellTower> cells;            // most recently seen first
    std::vector<CellEvent> events;           // newest first, at most kCellLogMax
};

constexpr size_t kCellLogMax = 300;
constexpr size_t kCellMax = 400;

inline std::string cellSummary(const CellTelemetry& t) {
    if (t.state == 0) return "waiting for samples";
    size_t ops = 0, named = 0;
    std::vector<std::string> seen;
    for (const auto& c : t.cells) {
        if (!c.hasSib1 || c.plmns.empty()) continue;
        named++;
        bool dup = false;
        for (const auto& s : seen) if (s == c.plmns[0]) dup = true;
        if (!dup) { seen.push_back(c.plmns[0]); ops++; }
    }
    char b[160];
    snprintf(b, sizeof b, "%zu cell%s, %zu identified, %zu operator%s", t.cells.size(), t.cells.size() == 1 ? "" : "s", named, ops, ops == 1 ? "" : "s");
    return b;
}

} // namespace dect2
