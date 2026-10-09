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

// One short line about the three decoders, e.g. "RTTY 45.45/170 locked · SSTV Robot 36 112/240 · FreeDV no sync"; "listening" when none is active.
inline std::string hfdigSummary(const HfdigTelemetry& t) {
    std::string out;
    auto add = [&](const char* s) { if (!out.empty()) out += " \xC2\xB7 "; out += s; };
    char b[96];
    if (t.rtty.state >= 1) {
        snprintf(b, sizeof b, "RTTY %g/%g %s", t.rtty.baud, t.rtty.shiftHz, t.rtty.state == 2 ? "receiving" : "locked");
        add(b);
    }
    if (t.sstv.state == 1) {
        snprintf(b, sizeof b, "SSTV %s %d/%d", t.sstv.mode.c_str(), t.sstv.lines, t.sstv.height);
        add(b);
    }
    const bool fdv = t.freedv.mode >= 0;
    if (fdv) { snprintf(b, sizeof b, "FreeDV %s sync", freedvModeName(t.freedv.mode)); add(b); }
    else if (!out.empty()) add(t.freedv.libFound ? "FreeDV no sync" : "FreeDV no library");
    return out.empty() ? "listening" : out;
}

} // namespace dect2
