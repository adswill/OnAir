// TETRA Direct Mode telemetry. RxTelemetry::tetradmo, engine standard 323, active standard 322 (family 321). Receive only.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct TetraDmoRow {
    int64_t wallTime = 0;
    double timeSec = 0;
    int frame = 0, slot = 0, msgType = -1;
    uint32_t dest = 0, src = 0, mni = 0;
    bool encrypted = false;
    std::string burst, text;
};

struct TetraDmoCall {
    uint32_t dest = 0, src = 0, mni = 0;
    int commType = 0, circuitMode = 0, priority = 0;
    bool group = true, active = false, encrypted = false, e2e = false, voice = false;
    int64_t wallStart = 0;
    double startSec = 0, durSec = 0;
    uint64_t voiceBursts = 0;
    std::string note;
};

struct TetraDmoTelemetry {
    uint64_t seq = 0;
    int state = 0;                     // 0 no input, 1 listening, 2 a DSB decoded in the last 5 s
    bool locked = false, dataValid = false;
    float levelDb = -200, snrDb = 0;
    double cfoHz = 0, timeSec = 0, inputRate = 0;
    uint64_t blocksOk = 0, blocksBad = 0, dsb = 0, dnb = 0, schF = 0, traffic = 0, trafficRefused = 0;
    int commType = -1, frame = 0, slot = 0, encState = -1;
    uint32_t lastSrc = 0, lastDest = 0, lastMni = 0;
    std::vector<TetraDmoRow> rows;     // newest first
    std::vector<TetraDmoCall> calls;   // active first, then the newest
    bool voiceCodec = false;           // the user-installed speech codec is in
    std::string note;
};

constexpr size_t kTetraDmoRowsMax = 400, kTetraDmoCallsMax = 100;

inline const char* tetraDmoCommName(int c) {
    switch (c) { case 0: return "direct MS-MS"; case 1: return "via DM-REP"; case 2: return "via DM-GATE"; case 3: return "via DM-REP/GATE"; default: return "-"; }
}

inline std::string tetraDmoSummary(const TetraDmoTelemetry& t) {
    char b[160];
    if (t.state == 0) return "TETRA DMO: waiting for samples";
    if (!t.dataValid) { snprintf(b, sizeof b, "TETRA DMO: listening (%.0f dBFS)", t.levelDb); return b; }
    int active = 0;
    for (const auto& c : t.calls) active += c.active;
    if (active) snprintf(b, sizeof b, "TETRA DMO: %d call%s on air, %llu DSB", active, active == 1 ? "" : "s", (unsigned long long)t.dsb);
    else snprintf(b, sizeof b, "TETRA DMO: %llu DSB, %llu DNB", (unsigned long long)t.dsb, (unsigned long long)t.dnb);
    return b;
}

} // namespace dect2
