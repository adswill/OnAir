// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NXDN receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One transmission: a voice call, a status message or a short data call
struct NxdnCall {
    int carrierId = 0; double carrierHz = 0;   // whole-span decoding: the carrier it came from (id 0: single channel), its frequency relative to the span centre
    int64_t wallTime = 0;        // Unix time of its start
    double startSec = 0, durSec = 0;
    bool active = false;
    int kind = 0;                // 0 voice, 1 status, 2 short data
    int src = -1, dst = -1;      // radio unit id, talkgroup (group calls) or unit; -1 not known yet
    int callType = 1;            // nxdn::callTypeName
    int cipher = 0, keyId = 0;   // cipher: 0 none, 1 scrambler, 2 DES, 3 AES (decrypted only with a user-supplied key, see `decrypt`)
    std::string decrypt;         // status text: ""/clear, "encrypted: add your key", "decrypted with key <name>", "key mismatch"
    int ran = -1;                // radio access number (from the SACCH / CAC)
    int baud = 0;                // 2400 (NXDN48) or 4800 (NXDN96)
    bool lateEntry = false;      // found from the SACCH superframe in the middle of the call (the header was missed)
    bool endSeen = false;        // TX_REL ended it (else the signal was lost)
    uint32_t voiceFrames = 0;
    bool voice = false;          // AMBE+2 decoded to sound
    std::string text;            // status: "status N"; short data: the text
    float berPct = 0;
};

struct NxdnSite {
    bool valid = false;
    uint32_t location = 0;
    int system = 0, site = 0, service = 0, channel = 0;
    int ran = -1;
    int64_t wallTime = 0;
};

struct NxdnTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 searching, 1 frames seen recently, 2 a call on air
    double cfoHz = 0;            // carrier offset measured on the frame sync
    float snrDb = 0;             // symbol SNR at the decision points
    float levelDb = -200;        // input level, dBFS
    bool dataValid = false;      // frames are being received
    uint64_t blocksOk = 0;       // channels (SACCH / FACCH1 / CAC) with a good CRC
    uint64_t blocksBad = 0;      // and with a bad one
    // details
    double timeSec = 0, inputRate = 0;
    int baud = 0;                // the rate of the last frames: 2400 or 4800
    float deviationHz = 0;       // the +3 symbol's deviation as measured
    bool inverted = false;       // the frequency comes upside down
    float berPct = 0;
    int ran = -1;
    uint64_t frames = 0, lichBad = 0, sacchOk = 0, sacchBad = 0, facchOk = 0, facchBad = 0, cacOk = 0, cacBad = 0;
    uint64_t voiceFrames = 0, voiceDecoded = 0, lateEntries = 0;
    bool ambe = false;           // mbelib found
    std::string ambePath;
    NxdnSite siteInfo;
    NxdnCall now;
    std::vector<NxdnCall> log;   // newest first, at most kNxdnLogMax: voice calls, status and short data
    std::vector<std::string> events;   // newest first: decoded control messages as text
};

constexpr size_t kNxdnLogMax = 300;

inline std::string nxdnId(int v) { return v < 0 ? std::string("?") : std::to_string(v); }

inline std::string nxdnSummary(const NxdnTelemetry& t) {
    char b[200];
    const NxdnCall& n = t.now;
    if (n.active) {
        snprintf(b, sizeof b, "%s -> %s%s, %.1f s", nxdnId(n.src).c_str(), n.callType == 1 ? "TG " : "", nxdnId(n.dst).c_str(), n.durSec);
        return b;
    }
    if (t.frames == 0) return "searching for NXDN";
    snprintf(b, sizeof b, "NXDN%d, %zu calls heard", t.baud == 4800 ? 96 : 48, t.log.size());
    return b;
}

} // namespace dect2
