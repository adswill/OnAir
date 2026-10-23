// COSPAS-SARSAT 406 MHz beacon receiver telemetry. RxTelemetry::sarsat, engine standard 94, active standard 93.
// Receive only; nothing is transmitted.
#pragma once
#include "dect2/sarsat/sarsat_proto.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct SarsatEntry {
    int64_t wallTime = 0;
    double timeSec = 0;
    sarsat::Beacon b;
    float snrDb = 0, levelDb = -200;
    float cfoHz = 0;
};

constexpr size_t kSarsatLogMax = 200;

struct SarsatTelemetry {
    uint64_t seq = 0;
    int state = 0;                 // 0 no input, 1 listening, 2 a beacon decoded in the last 5 minutes
    double cfoHz = 0;              // of the last burst
    float snrDb = 0;               // of the last burst (carrier to noise in 31 Hz)
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // bursts decoded / bursts seen but the checks failed
    uint64_t bursts = 0;
    double timeSec = 0, inputRate = 0;
    double wideSpanHz = 0;         // the whole-capture search covers this much (0: off)
    int wideChannels = 0;
    std::vector<SarsatEntry> log;  // newest first
};

inline std::string sarsatSummary(const SarsatTelemetry& t) {
    char b[200];
    if (t.state == 0) return "COSPAS-SARSAT: waiting for samples";
    if (t.log.empty()) {
        if (t.bursts) { snprintf(b, sizeof b, "COSPAS-SARSAT: %llu bursts heard, none decoded", (unsigned long long)t.bursts); return b; }
        snprintf(b, sizeof b, "COSPAS-SARSAT: listening (%.0f dBFS)", t.levelDb);
        return b;
    }
    const auto& e = t.log.front();
    snprintf(b, sizeof b, "Beacon %s %s%s%s", e.b.hexId.c_str(), e.b.beaconType.c_str(), e.b.country.empty() ? "" : ", ", e.b.country.c_str());
    return b;
}

} // namespace dect2
