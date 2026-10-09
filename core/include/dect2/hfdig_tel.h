// HF digital receiver telemetry, shared by the receiver and the UI: the receiver's own members, then each decoder's telemetry by value
// (defined in hfdig_rtty.h, hfdig_sstv.h and hfdig_freedv.h).
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include "hfdig_rtty.h"
#include "hfdig_sstv.h"
#include "hfdig_freedv.h"
#include <cstdint>
#include <cstdio>
#include <string>

namespace dect2 {

struct HfdigTelemetry {
    uint64_t seq = 0;            // grows with every report
    int state = 0;               // 0 no decoder yet (the audio is made and handed to the decoders)
    double cfoHz = 0;            // carrier error
    float snrDb = 0;
    float levelDb = -200;        // power of the whole input, dBFS (-200 = nothing measured yet)
    bool dataValid = false;      // something has been decoded
    uint64_t blocksOk = 0;       // good and bad blocks of the decoders
    uint64_t blocksBad = 0;
    // set-up
    double timeSec = 0;          // signal seconds since the start (now)
    double inputRate = 0;
    float audioDb = -200;        // power of the upper sideband (200 .. 3800 Hz) before the AGC, dBFS
    // the decoders
    HfdigRttyTelemetry rtty;
    HfdigSstvTelemetry sstv;
    HfdigFreedvTelemetry freedv;
};

inline std::string hfdigSummary(const HfdigTelemetry& t) {
    char b[128];
    snprintf(b, sizeof b, "HF digital: no decoder yet, sideband %.1f dBFS, %.1f s of signal", t.audioDb, t.timeSec);
    return b;
}

} // namespace dect2
