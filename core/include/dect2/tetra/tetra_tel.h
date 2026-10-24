// TETRA receiver telemetry, shared by the receiver and the UI.
// Only what a TETRA base station broadcasts in the clear to every terminal is here: the network identity (MCC, MNC), colour code, location
// area, the cell's carriers and services, its neighbour cells and whether the network uses air interface encryption, and the groups (GSSI) clear group
// calls are set up for. No subscriber identity, no call, no short data content and no voice is decoded or kept; encrypted signalling is only counted.
// The first members of TetraTelemetry are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct TetraNeighbourInfo {
    int cellId = 0, carrier = 0, la = -1, cellLoad = 0, mcc = -1, mnc = -1;
    bool synced = false;
    double freqHz = 0;            // from the carrier number and band (0: unknown)
};

// A group (GSSI) a group call was set up for in the clear signalling of a cell (D-SETUP): the address of the group, never of a terminal
struct TetraGroupInfo {
    int gssi = 0;
    int enc = -1;                 // the end-to-end encryption flag of the last set-up: 0 clear, 1 encrypted, -1 not stated
    uint64_t calls = 0;           // call set-ups heard (each call identifier once)
    int lastCallId = -1;
    int64_t firstWall = 0, lastWall = 0;
};

// One TETRA carrier (main control channel or another carrier of the cell), keyed by its frequency and network identity
struct TetraCell {
    double freqHz = 0;            // measured: the radio's centre plus the offset found (the input offset for files)
    double nominalHz = 0;         // from SYSINFO (main carrier, band and offset) when that is this carrier, else freqHz
    double cfoHz = 0;             // measured minus nominal
    // SYNC (BSCH) and D-MLE-SYNC
    int mcc = -1, mnc = -1, colourCode = -1, systemCode = 0, sharingMode = 0, tsReservedFrames = 0;
    bool uplaneDtx = false, frame18Ext = false, lateEntry = false;
    int nbrBroadcast = 0, cellLoad = 0;
    int tn = 0, fn = 0, mn = 0;   // at the last synchronisation burst
    // SYSINFO (BNCH) and D-MLE-SYSINFO
    bool hasSysinfo = false;
    int mainCarrier = 0, band = 0, offset = 0, duplex = 0;
    bool reverse = false;
    double mainHz = 0, uplinkHz = 0;   // the cell's main carrier and its uplink (0 unknown)
    bool isMain = false;               // this carrier is the main carrier (MCCH)
    int numScch = 0, msTxPwrMax = 0, rxLevAccessMin = 0, accessParam = 0, dlTimeout = 0;
    bool cckFlag = false; int hyperframeOrCck = 0;
    int optFlag = 0; uint32_t optValue = 0;
    int la = -1, subscriberClass = 0, bsService = 0;
    bool secValid = false, authRequired = false, secClass1 = false, secClass2 = false, secClass3 = false;
    bool aie = false;                  // the cell's "air interface encryption service" bit (D-MLE-SYSINFO): the cell supports class 2 / 3 MS
    uint64_t encryptedPdus = 0;        // MAC-RESOURCE PDUs whose encryption mode is not 0 (counted, never read); blocks that fail the CRC are blocksBad, not this
    // What the cell advertises (EN 300 392-7 A.8.77 with the service bit): 0 not known yet (no SYSINFO), 1 none advertised (class 1 only), 2 class 2,
    // 3 class 3, 4 encryption service available, class not heard yet. Scrambling (derived from MCC, MNC and colour code) is not encryption.
    int airEnc() const { return !hasSysinfo ? 0 : !aie ? 1 : secClass3 ? 3 : secClass2 ? 2 : 4; }
    bool encryption() const { return airEnc() >= 2 || encryptedPdus > 0; }
    // MCCH: D-NWRK-BROADCAST
    bool hasNwrk = false;
    int reselect = 0;
    std::string netTime;
    std::vector<TetraNeighbourInfo> neighbours;
    // CMCE: the groups clear group calls were set up for, most calls first
    std::vector<TetraGroupInfo> groups;
    // ACCESS-ASSIGN: downlink use of timeslots 1 - 4 (-1 not seen; see tetra::dlUsageText)
    int tsUsage[4] = {-1, -1, -1, -1};
    uint64_t clearPdus = 0;            // clear MLE messages seen (counted only)
    // measurements (dBFS: relative to the radio's full scale, not calibrated)
    float levelDb = -200, snrDb = -99;
    int64_t firstWall = 0, lastWall = 0;
    double lastSec = 0;
    uint64_t seen = 0;                 // capture blocks in which it was found
    uint64_t bschOk = 0, bnchOk = 0, blocksOk = 0, blocksBad = 0;
};

struct TetraEvent { int64_t wallTime = 0; double sec = 0; std::string text; };
struct TetraCarrierSeen { double freqHz = 0; float levelDb = 0; bool synced = false; };   // a 25 kHz hump in the spectrum

struct TetraTelemetry {
    uint64_t seq = 0;
    int state = 0;                // 0 no input yet, 1 searching, 2 a cell was found in the last 30 s
    double cfoHz = 0;             // of the strongest cell
    float snrDb = 0;              // of the strongest cell
    float levelDb = -200;         // input power, dBFS
    bool dataValid = false;       // a cell was found in the last 30 s
    uint64_t blocksOk = 0;        // signalling blocks decoded (CRC good)
    uint64_t blocksBad = 0;       // signalling blocks that failed their CRC
    double timeSec = 0;
    double inputRate = 0;
    double centerHz = 0;          // the radio's centre (0: unknown)
    uint64_t blocks = 0;          // capture blocks searched
    std::vector<TetraCarrierSeen> carriers;   // in the last block
    std::vector<TetraCell> cells;             // most recently seen first
    std::vector<TetraEvent> events;           // newest first, at most kTetraLogMax
};

constexpr size_t kTetraLogMax = 300;
constexpr size_t kTetraMax = 200;

inline std::string tetraSummary(const TetraTelemetry& t) {
    if (t.state == 0) return "waiting for samples";
    size_t named = 0, enc = 0;
    for (const auto& c : t.cells) { if (c.mcc >= 0) named++; if (c.encryption()) enc++; }
    char b[160];
    snprintf(b, sizeof b, "%zu TETRA carrier%s, %zu identified, %zu with encryption", t.cells.size(), t.cells.size() == 1 ? "" : "s", named, enc);
    return b;
}

} // namespace dect2
