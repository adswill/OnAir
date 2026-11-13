// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MAC television receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct MacTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 searching, 1 line sync (data bursts found), 2 frame sync (line 625 found): pictures and packets decoded
    double cfoHz = 0;            // carrier offset, from the clamp level (13.5 MHz/V)
    float snrDb = 0;             // data eye: the duobinary levels against the rms noise at the decision points
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // packet headers decoded (Golay corrected included) / line-625 blocks that failed their BCH check

    // what was found
    int variant = -1;            // mac::Variant: 0 D2-MAC, 1 D-MAC; -1 not yet
    std::string variantName;     // "D2-MAC/packet", "D-MAC/packet"
    bool wide = false;           // 16:9 (SDF MVSCG aspect bit, VCONF)
    bool aspectKnown = false;
    bool scrambled = false;      // the vision is scrambled (VSAM) or the service is under conditional access (ACCM)
    bool controlledAccess = false;
    std::string scrambling;      // "free access, unscrambled", "controlled access, single-cut line rotation", ...
    std::string caSystem;        // "EuroCrypt" (from the ACCM CA byte), "" none
    uint64_t ecmPackets = 0;     // packets on the ECM address
    bool soundScrambled = false; // the TV sound is in the conditional-access group: not played
    // service identification
    std::string channelName;     // the TV service name (SREF), else the network name
    std::string networkName;     // NWNAME
    std::string country;         // NWO text
    int satChannel = -1;         // NWO: satellite channel number
    uint16_t chid = 0;           // SDF channel identification
    bool siSeen = false;
    uint64_t siGroupsOk = 0, siGroupsBad = 0;
    // sync and timing
    float syncQuality = 0;       // share of lines whose line sync word was found
    double lineHz = 0, lineErrPpm = 0;
    uint64_t lines = 0, frames = 0, fswFound = 0;
    int fcnt = -1;               // RDF frame counter of the last frame
    bool inverted = false;       // the spectrum is inverted (the picture came out negative; corrected)
    // levels
    float videoSnrDb = 0;        // line 624 references against their rms noise
    float blackLevel = 0, whiteLevel = 1;   // line 624, after the clamp (nominal 0 and 1 V)
    float dataAmp = 0;           // measured duobinary extreme level (nominal 0.4 V)
    float deviationMhzPerV = 0;  // what the levels say about the deviation (13.5 nominal)
    float levelDb = -200;        // input level, dBFS
    double inputRate = 0;
    // packets
    uint64_t packets = 0, packetsCorrected = 0, dummyPackets = 0;
    float bitErrorRate = 0;      // from the frame sync sequence of line 625
    // sound
    int soundAddress = -1;
    bool soundPresent = false;
    std::string soundMode;       // "companded 32 kHz mono", ...
    float soundLevelDb = -120;
    uint64_t soundPackets = 0, soundErrors = 0;
    // scope: one line of the baseband (512 points over 1296 samples, 0..1 V), and the data burst eye (the decision samples, last line)
    std::vector<float> lineWave;
    std::vector<float> eye;
};

inline std::string macSummary(const MacTelemetry& t) {
    char b[300];
    if (t.state == 0) snprintf(b, sizeof b, "MAC: searching");
    else snprintf(b, sizeof b, "MAC: %s%s  %s  %s  data SNR %.1f dB  packets %llu  %s%s", t.variantName.c_str(), t.state == 2 ? "" : " (line sync only)",
                  t.channelName.empty() ? "(no name yet)" : t.channelName.c_str(), t.wide ? "16:9" : "4:3", t.snrDb, (unsigned long long)t.packets,
                  t.scrambled ? "scrambled" : "clear", t.scrambled && !t.caSystem.empty() ? (" (" + t.caSystem + ")").c_str() : "");
    return b;
}

} // namespace dect2
