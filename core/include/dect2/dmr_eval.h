// Compare what the DMR receiver reported with what the test signal sent (tests and `dmrtool check`).
#pragma once
#include "dmr_gen.h"
#include "dmr_tel.h"
#include <functional>
#include <string>
#include <vector>

namespace dect2 {

struct DmrScore {
    int voiceSent = 0, voiceFound = 0;        // group, private and all calls that ended before the cut-off; found with the right kind, IDs and about the right length
    int idsWrong = 0;                         // calls found at the right time with wrong or missing IDs
    int aliasSent = 0, aliasFound = 0;
    int messagesSent = 0, messagesFound = 0;  // text messages with identical text and a good CRC
    int controlSent = 0, controlFound = 0;
    int falseCalls = 0;                       // log entries that match nothing that was sent
    double frameRatio = 0;                    // voice frames counted / expected, over the found calls
    std::string report;
};

// `secs`: the signal time that has been fed to the receiver; entries sent in the last `margin` seconds are not counted (they may not have finished).
// `skip`: entries sent before this time are not counted (the receiver was not locked yet, or had been disturbed).
// `offset`: the receiver's clock minus the generator's clock for the same instant (the filters delay the signal by a few tens of ms).
DmrScore dmrScore(const std::vector<DmrTruth>& truth, const DmrTelemetry& t, double secs, double margin = 3.0, double skip = 0.0, double offset = 0.0);

// ---- a whole run: the test signal (with whatever `mod` does to it), rounded to 8 bits like a HackRF, into a receiver, in chunks
struct DmrScenario {
    DmrGenConfig cfg;
    double secs = 14;
    size_t chunk = 65536;
    bool quantise = true;
    double resetAt = -1;         // call reset() on the receiver at the first chunk boundary after this time
    double skip = 2.5;           // entries sent before this time are not scored
    std::function<void(cf32* x, size_t n, size_t first, double rate)> mod;    // after generation, before the 8 bit rounding
};

struct DmrRunResult {
    DmrTelemetry tel;
    DmrScore sc;
    std::vector<DmrTruth> truth;
    double cpu = 0;              // seconds spent in feed()
    bool seqOk = true;           // the telemetry sequence only grew
    double lockedAt = -1;        // signal time of the first report with state 2
};

DmrRunResult dmrRun(const DmrScenario& s);

} // namespace dect2
