// GSM-R receiver telemetry, shared by the receiver and the UI.
// Only what a GSM / GSM-R base station broadcasts to every mobile on its BCCH is here: its identity (BSIC, cell identity, location area,
// operator code), its access parameters and the channel lists of System Information 1 to 4.
// The first members of GsmrTelemetry are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include "dect2/gsmr/gsmr_si.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One base station carrier (its BCCH), keyed by its ARFCN and BSIC
struct GsmrCell {
    double freqHz = 0;            // the nominal downlink frequency of the ARFCN (the raster frequency)
    int arfcn = -1;               // -1: not on the 900 MHz raster
    bool railway = false;         // inside the GSM-R downlink band (921 - 925 MHz, ARFCN 955 - 973)
    int bsic = -1, ncc = 0, bcc = 0;
    int fn = -1;                  // the last TDMA frame number read from the SCH
    // System Information 3 / 4
    bool hasLai = false;
    int mcc = -1, mnc = -1, mncDigits = 2;
    uint32_t lac = 0;
    std::string op;               // from the MCC-MNC table ("" unknown)
    bool hasSi3 = false;
    uint32_t cellId = 0;
    bool attach = false;          // IMSI attach / detach allowed
    int agBlks = 0, ccchConf = 0, pagMfrms = 0, t3212 = 0;   // control channel description (t3212 in decihours, 0 = no periodic update)
    int rlTimeout = 0, dtx = 0, pwrc = 0;                     // cell options
    int reselHystDb = 0, msTxPwrMax = 0, rxLevAccessMin = 0;   // cell selection (dB, power control level, dBm)
    bool neci = false, acs = false;
    int maxRetrans = 0, txInteger = 0;
    bool cellBar = false;
    uint16_t acBarred = 0;        // access classes 0 - 15 barred (bit n = class n), class 10 is the emergency flag
    // System Information 1 / 2
    bool hasSi1 = false, hasSi2 = false;
    std::vector<int> cellAlloc;   // ARFCNs of the cell allocation (SI1)
    std::vector<int> neigh;       // ARFCNs of the BCCH neighbours (SI2)
    std::vector<int> neighBis;    // more neighbours from SI2bis (the other band of a dual band network)
    gsmr::CcchCount ccch;         // common control channel blocks by message kind (the cell's paging and access load; no contents kept)
    int nccPermitted = 0;         // bit mask (SI2)
    std::vector<int> typesSeen;   // RR message types decoded (0x19 SI1, 0x1A SI2, 0x1B SI3, 0x1C SI4, others)
    // measurements (dBFS: relative to the radio's full scale, not calibrated)
    float levelDb = -200, snrDb = -99;
    double cfoHz = 0;
    int64_t firstWall = 0, lastWall = 0;
    double lastSec = 0;
    uint64_t seen = 0;            // blocks in which it was found
    uint64_t siMsgs = 0;          // system information messages decoded
    uint64_t siBad = 0;           // BCCH blocks whose FIRE code check failed
};

struct GsmrEvent { int64_t wallTime = 0; double sec = 0; std::string text; };
struct GsmrCarrierSeen { double freqHz = 0; float levelDb = 0; bool synced = false; };   // a 200 kHz hump in the spectrum

struct GsmrTelemetry {
    uint64_t seq = 0;
    int state = 0;                // 0 no input yet, 1 searching, 2 a cell was found in the last 30 s
    double cfoHz = 0;             // of the strongest cell
    float snrDb = 0;              // of the strongest cell
    float levelDb = -200;         // input power, dBFS
    bool dataValid = false;       // a cell was found in the last 30 s
    uint64_t blocksOk = 0;        // system information messages decoded
    uint64_t blocksBad = 0;       // BCCH blocks that failed their check
    double timeSec = 0;
    double inputRate = 0;
    double centerHz = 0;          // the radio's centre (0: unknown)
    double dialOffsetHz = 0;
    uint64_t blocks = 0;          // capture blocks searched
    uint64_t retunes = 0;
    double lastBlockCenterHz = 0;
    std::vector<GsmrCarrierSeen> carriers;   // in the last block
    std::vector<GsmrCell> cells;             // most recently seen first
    std::vector<GsmrEvent> events;           // newest first, at most kGsmrLogMax
};

constexpr size_t kGsmrLogMax = 300;
constexpr size_t kGsmrMax = 200;

inline std::string gsmrSummary(const GsmrTelemetry& t) {
    if (t.state == 0) return "waiting for samples";
    size_t named = 0, rail = 0;
    for (const auto& c : t.cells) { if (c.hasLai) named++; if (c.railway) rail++; }
    char b[160];
    snprintf(b, sizeof b, "%zu cell%s, %zu identified, %zu in the railway band", t.cells.size(), t.cells.size() == 1 ? "" : "s", named, rail);
    return b;
}

} // namespace dect2
