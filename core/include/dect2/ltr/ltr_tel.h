// LTR (Logic Trunked Radio) telemetry. RxTelemetry::ltr, engine standard 131, active standard 130 (family 129).
// Control data only: the 300 bit/s subaudible words. No voice is decoded. Receive only.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct LtrMessage {
    int64_t wallTime = 0;
    double timeSec = 0;
    int area = 0, gotoCh = 0, home = 0, id = 0, freeCh = 0;   // the 24 information bits
    int chk = 0;                                               // the 7 check bits as received (a word is only listed when they verify)
    bool chkOk = false;                                        // the check bits matched (always true for listed rows)
    bool plausible = false;                                    // the fields are in their documented ranges
    std::string talkgroup;                                     // A-HH-GGG
    std::string kind;                                          // idle, callsign ID, busy, token, or empty
    int carrierId = 0;          // whole-span decoding: 1 when the row came from a carrier of the span (carrierHz: its offset from the centre)
    double carrierHz = 0;
};

struct LtrRepeater {
    int ch = 0;                    // repeater number 1 - 20
    uint64_t asGoto = 0, asHome = 0, asFree = 0;
    double lastSec = -1;
};

struct LtrTelemetry {
    uint64_t seq = 0;
    int state = 0;                 // 0 no input, 1 listening, 2 a good word in the last 15 s
    bool locked = false;
    double cfoHz = 0;              // not measured (0)
    float snrDb = 0, levelDb = -200, deviationHz = 0;   // deviationHz: peak-to-peak size of the subaudible data
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // words with the sync, matching check bits and plausible fields / sync found but check bits or fields wrong
    uint64_t syncs = 0;
    int area = -1;                 // area bit of the last good word
    double timeSec = 0, inputRate = 0, centerHz = 0;
    std::vector<LtrMessage> messages;       // newest first
    std::vector<LtrRepeater> repeaters;     // numbers seen, ascending
};

constexpr size_t kLtrLogMax = 400;

inline std::string ltrSummary(const LtrTelemetry& t) {
    char b[160];
    if (t.state == 0) return "LTR: waiting for samples";
    if (!t.dataValid) { snprintf(b, sizeof b, "LTR: listening (%.0f dBFS)", t.levelDb); return b; }
    snprintf(b, sizeof b, "LTR: %s, %llu words, %zu repeaters", t.locked ? "locked" : "no lock", (unsigned long long)t.blocksOk, t.repeaters.size());
    return b;
}

} // namespace dect2
