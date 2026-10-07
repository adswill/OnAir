// DMR receiver telemetry, shared by the receiver and the UI.
#pragma once
#include "ring.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One call or message in the log (the last 100 are kept)
struct DmrCall {
    int slot = 0;                // 1 or 2; 0 when the slot could not be told
    double startSec = 0, endSec = 0;   // seconds of signal since the receiver started (or was reset)
    bool active = false;         // still going on
    int cc = -1;                 // colour code
    int kind = 0;                // 0 group voice, 1 private voice, 2 all call, 3 data message, 4 control (CSBK)
    uint32_t src = 0, dst = 0;   // 0 when not (yet) known
    bool idsKnown = false;
    int voiceFrames = 0;         // vocoder frames counted (three per voice burst)
    int fecErrors = 0;           // bit errors repaired plus blocks lost
    bool emergency = false, privacy = false;
    bool lateEntry = false;      // joined after the start: no voice header seen
    bool terminated = false;     // the end was announced by a terminator (otherwise the signal just stopped)
    std::string alias;           // talker alias
    std::string note;            // for data and control: what it was
};

struct DmrMessage {
    double sec = 0;
    int slot = 0;
    uint32_t src = 0, dst = 0;
    bool group = false;
    bool crcOk = false;
    std::string format;          // "defined short data, UTF-8", ...
    std::string text;
};

struct DmrSlot {
    bool active = false;         // bursts decode on this slot
    int state = 0;               // 0 none, 1 voice, 2 data, 3 idle, 4 control
    std::string lastBurst;       // "Voice A", "CSBK", "Idle", ...
    bool inCall = false;
    int callKind = 0;            // as DmrCall::kind
    uint32_t src = 0, dst = 0;
    int voiceFrames = 0;         // of the current call
    float rmsErr = 0;            // eye quality: rms distance of the symbols from their level, in units of the level spacing (0.25 is poor, 0.1 good)
    float ber = 0;               // bit error rate estimated from what the codes repaired
    uint64_t bursts = 0;
};

struct DmrTelemetry {
    // ---- the part every mode has: the engine copies it into RxTelemetry; keep these members and what they mean
    uint64_t seq = 0;            // grows with every report and never restarts (not even after reset())
    int state = 0;               // 0 searching, 1 partly locked, 2 locked and decoding
    double cfoHz = 0;            // carrier offset
    float snrDb = 0;             // signal-to-noise ratio of the 4-level symbols (distance of the symbols from their levels)
    bool dataValid = false;      // bursts are decoding (voice, data or control)
    uint64_t blocksOk = 0, blocksBad = 0;   // FEC blocks decoded well and badly since the start: BPTC, Reed-Solomon, trellis, embedded signalling, slot types

    // ---- this mode's own fields below
    int cc = -1;                 // colour code of the channel (-1: not known yet)
    std::string link;            // "base station", "mobile", "direct mode": what the sync patterns say
    int slotsLocked = 0;         // number of time slots being followed
    DmrSlot slot[2];             // time slot 1 and 2
    float levelDbfs = -120;      // power in the 48 kHz channel
    float noiseDbfs = -120;      // noise floor tracked beside the channel (-120 when not known)
    float cnrDb = 0;             // carrier to noise ratio in 12.5 kHz when the noise could be measured
    float devHz = 0;             // measured deviation of the outer symbols (+-3 level), nominal 1944 Hz
    float symbolPpm = 0;         // timing error of the symbol clock against the receiver clock
    float ber = 0;               // estimated bit error rate over all codes
    uint64_t syncCount[9] = {};  // frame syncs by pattern: BS voice, BS data, MS voice, MS data, RC, direct 1 voice, 1 data, 2 voice, 2 data
    uint64_t burstCount[16] = {};   // data bursts by data type (PI header ... unified single block data)
    uint64_t voiceBursts = 0, embeddedBursts = 0, rcBursts = 0, unknownBursts = 0, idleOk = 0;
    // FEC statistics
    uint64_t bptcOk = 0, bptcFixed = 0, bptcFail = 0;     // bursts decoded clean, with repaired bits, and lost
    uint64_t golayFixed = 0, golayFail = 0;               // slot types
    uint64_t rsOk = 0, rsFixed = 0, rsFail = 0;           // voice header and terminator link control
    uint64_t crcOk = 0, crcBad = 0;                       // CSBK, headers, data block CRCs
    uint64_t trellisOk = 0, trellisFail = 0;
    uint64_t embOk = 0, embFail = 0, embLcOk = 0, embLcFail = 0;
    uint64_t calls = 0;          // calls and messages seen
    std::string cachInfo;        // short LC from the CACH of a base station, e.g. "activity: slot 1 group voice, slot 2 none"
    // symbols: the last 800 decided symbol values scaled so that the nominal levels are -3, -1, +1, +3, and their histogram (bins of 0.25 from -5 to +5)
    std::vector<float> eye;
    uint32_t levelHist[40] = {};
    uint64_t levelCount[4] = {}; // decisions by level: -3, -1, +1, +3
    std::vector<float> spectrumDb;   // power spectrum of the channel, 128 bins from -12 to +12 kHz, in dB relative to the strongest
    std::vector<DmrCall> callLog;    // oldest first, at most 100
    std::vector<DmrMessage> messages;   // oldest first, at most 16
};

// One line for the command line and the log
inline std::string dmrSummary(const DmrTelemetry& t) {
    static const char* st[5] = {"-", "voice", "data", "idle", "ctrl"};
    char b[320];
    snprintf(b, sizeof b, "DMR: %s %s CC %d  S1 %s S2 %s  SNR %.1f dB  CFO %+.0f Hz  calls %llu  ok %llu bad %llu",
             t.state == 2 ? "locked" : t.state == 1 ? "partial" : "searching", t.link.c_str(), t.cc, st[t.slot[0].state % 5], st[t.slot[1].state % 5], t.snrDb, t.cfoHz,
             (unsigned long long)t.calls, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    return b;
}

} // namespace dect2
