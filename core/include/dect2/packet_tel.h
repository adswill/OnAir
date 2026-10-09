// APRS / Packet receiver telemetry, shared by the receiver and the UI. Skeleton: only the generic members the engine and the
// interface need; the mode's own results go below them.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace dect2 {

struct PacketTelemetry {
    uint64_t seq = 0;            // grows with every report
    int state = 0;               // 0 no decoder yet (the receiver only measures the level)
    double cfoHz = 0;            // carrier error
    float snrDb = 0;
    float levelDb = -200;        // power of the whole input, dBFS (-200 = nothing measured yet)
    bool dataValid = false;      // something has been decoded
    uint64_t blocksOk = 0;       // good and bad blocks (frames, codewords) of the mode
    uint64_t blocksBad = 0;
    // set-up
    double timeSec = 0;          // signal seconds since the start (now)
    double inputRate = 0;
};

inline std::string packetSummary(const PacketTelemetry& t) {
    char b[128];
    snprintf(b, sizeof b, "APRS / Packet: no decoder yet, level %.1f dBFS, %.1f s of signal", t.levelDb, t.timeSec);
    return b;
}

} // namespace dect2
