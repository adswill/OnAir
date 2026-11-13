// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF ALE receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One decoded word
struct AleWordLog {
    double utc = 0;
    int type = 0;                // ale::WordType
    std::string text;            // its three characters (non-printing ones as '.')
    int voteErrs = 0, golayErrs = 0;
};

// One call (a transmission put together), or a sounding
struct AleEvent {
    double utc = 0;              // its first word
    double dialHz = 0;           // the dial frequency it was heard on (0: a file)
    std::string from, to;        // to: the called addresses, ", " between them
    std::string type;            // "Sounding", "Individual", "Net", "Group", "AllCall", "AnyCall", "Response", "Ack", "Net response"
    bool tis = false;            // concluded TIS (answer requested), else TWS
    std::string amd;             // AMD text
    std::string cmd;             // LQA report and other commands
    float ber = 0;               // bits not unanimous in the vote / bits received (the raw channel bit error rate)
    float snrDb = 0;             // in 3 kHz
    float cfoHz = 0;
    int words = 0;
};

// A station heard (a caller, a responder or a sounder)
struct AleStation {
    std::string address;
    double lastUtc = 0;
    double lastDialHz = 0;
    int count = 0;
    float snrDb = 0, ber = 0;
    std::string lastType;
};

constexpr size_t kAleKeep = 500;        // events kept
constexpr size_t kAleWordKeep = 400;    // words kept

struct AleTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 nothing yet, 1 searching, 2 in word sync
    double cfoHz = 0;            // the tone frequency error tracked (AFC)
    float snrDb = 0;             // of the newest word, in 3 kHz
    float levelDb = -200;
    bool dataValid = false;      // a word in the last minute
    uint64_t blocksOk = 0;       // words decoded
    uint64_t blocksBad = 0;      // words missed in sync
    // details
    double timeSec = 0, inputRate = 0;
    double nowUtc = 0;
    int timeMode = 0;            // 0 system clock, 1 the given start time
    double dialHz = 0;
    float ber = 0;               // of the newest word
    std::vector<AleWordLog> words;       // oldest first
    std::vector<AleEvent> events;        // oldest first
    std::vector<AleStation> stations;    // by last heard, newest first
};

inline std::string aleSummary(const AleTelemetry& t) {
    char b[240];
    if (t.events.empty()) snprintf(b, sizeof b, "%s, %llu words", t.state == 2 ? "in sync" : "listening", (unsigned long long)t.blocksOk);
    else {
        const AleEvent& e = t.events.back();
        snprintf(b, sizeof b, "%zu calls, %zu stations; %s %s%s%s", t.events.size(), t.stations.size(), e.type.c_str(), e.from.c_str(),
                 e.to.empty() ? "" : " -> ", e.to.c_str());
    }
    return b;
}

} // namespace dect2
