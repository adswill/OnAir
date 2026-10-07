// DTMB receiver telemetry, shared by the receiver and the UI.
#pragma once
#include "ring.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct DtmbTelemetry {
    // ---- the part every mode has: the engine copies it into RxTelemetry; keep these members and what they mean
    uint64_t seq = 0;            // grows with every report and never restarts (not even after reset())
    int state = 0;               // 0 searching, 1 partly locked, 2 locked and decoding
    double cfoHz = 0;            // carrier offset
    float snrDb = 0;             // signal-to-noise ratio of what the mode decodes (symbols, cells, bits: whatever it measures best)
    bool dataValid = false;      // payload is coming out (packets, sound, messages)
    uint64_t blocksOk = 0, blocksBad = 0;   // FEC blocks, frames or messages decoded well and badly since the start (the history plot and the quality bar use them)

    // ---- this mode's own fields below
    // 0 searching for PN headers; 1 frames are tracked but the system information is not decoded or no codeword has come out yet; 2 transport stream flowing
    int header = -1;             // frame header: 0 PN420, 1 PN595, 2 PN945, -1 not known yet
    bool phaseRotates = true;    // the PN phase changes from frame to frame
    int carriers = 3780;         // 3780 (multi-carrier) or 1 (single carrier); 0 while it is not known (a PN595 signal before its system information)
    bool siOk = false;           // the system information has been decoded
    int siIndex = 0;             // 3 .. 24 when siOk
    int mapping = -1;            // 0 4QAM-NR, 1 4QAM, 2 16QAM, 3 32QAM, 4 64QAM
    int rate = -1;               // 0 = 0.4, 1 = 0.6, 2 = 0.8
    int interleaver = 0;         // 1 (M = 240) or 2 (M = 720)
    float siScore = 0;           // correlation of the system information with the best of the 22 words (1 = perfect)
    float merDb = 0;             // modulation error ratio of the equalised data carriers (decision based: meaningful above about 6 dB)
    float snrPnDb = 0;           // carrier to noise from the PN header fit, valid at any level
    double clockPpm = 0;         // sample clock offset found by the timing loop
    float echoSpanUs = 0;        // delay between the first and the last significant path
    float peakOffsetUs = 0;      // lag of the strongest path from the nominal position
    float levelDbfs = -120;      // signal level at the input
    uint64_t frames = 0;         // signal frames processed since the start
    std::vector<cf32> cells;     // equalised data cells of the last frame (at most 2048, unit power scale)
    std::vector<float> cirDb;    // channel impulse response: magnitude in dB below the strongest tap, one value per symbol (0.1323 us)
    int cirFirst = 0;            // lag in symbols of cirDb[0]
    float ldpcIter = 0;          // recent mean LDPC iterations per codeword
    uint64_t packets = 0;        // transport stream packets delivered
    uint64_t cwDropped = 0;      // codewords dropped because the decoder was behind
    uint64_t cwSkipped = 0;      // codewords not tried: after a long run of failures only every eighth is decoded
    uint64_t bchCorrected = 0;   // BCH blocks with one corrected bit
    bool tsLock = false;         // transport packets with good sync bytes are coming out
    float netMbps = 0;           // payload bit rate of the signalled mode
    float frameLossPct = 0;      // share of recent frames without a usable header
    bool rateOk = true;
};

// One line for the command line and the log
inline std::string dtmbSummary(const DtmbTelemetry& t) {
    static const char* hdr[3] = {"PN420", "PN595", "PN945"};
    static const char* map[5] = {"4QAM-NR", "4QAM", "16QAM", "32QAM", "64QAM"};
    static const char* rt[3] = {"0.4", "0.6", "0.8"};
    char b[256];
    if (t.state == 0) snprintf(b, sizeof b, "DTMB: searching");
    else if (!t.siOk) snprintf(b, sizeof b, "DTMB: %s frames tracked, system information not decoded  C/N %.1f dB  CFO %.0f Hz", t.header >= 0 ? hdr[t.header] : "?", t.snrPnDb, t.cfoHz);
    else snprintf(b, sizeof b, "DTMB%s: %s %s %s mode %d  C/N %.1f dB  MER %.1f dB  CFO %.0f Hz  LDPC ok %llu bad %llu  packets %llu%s", t.carriers == 1 ? " C=1" : "", hdr[t.header], map[t.mapping], rt[t.rate], t.interleaver, t.snrPnDb,
                  t.merDb, t.cfoHz, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.packets, t.tsLock ? "  TS lock" : "");
    return b;
}

} // namespace dect2
