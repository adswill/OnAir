// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NOAA Weather Radio SAME / EAS telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include "same_proto.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kSameAlertMax = 100;
constexpr size_t kSameToneMax = 40;

// One decoded message (an alert, or the end of one)
struct SameAlert {
    int64_t wall = 0;               // computer clock (UTC seconds) when it was decoded
    double timeSec = 0;             // signal seconds
    bool eom = false;               // "NNNN": the end of a message
    bool valid = false;             // all fields right; false: the raw text only (a damaged message)
    std::string raw;                // what the vote gave, as text
    std::string err;                // why it is not valid
    int bursts = 0;                 // repeats heard (3 are sent)
    int agreeing = 0;               // repeats identical to the result
    int bitsFixed = 0;              // bits the vote changed against some repeat
    float snrDb = 0;
    double cfoHz = 0;
    double toneSec = 0;             // length of the 1050 Hz attention tone that followed (0 none)
    int year = 0;                   // the computer clock's year when decoded (the message has day of year only)
    same::Message msg;              // the parsed fields (valid only)
};

struct SameTone {
    int64_t wall = 0;
    double timeSec = 0;
    double durSec = 0;
    float levelDb = -200;
};

struct SameTelemetry {
    uint64_t seq = 0;
    int state = 0;                  // 0 nothing, 1 carrier (tone or data bursts) lately, 2 a message decoded in the last minutes
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0;          // messages decoded (alerts and ends)
    uint64_t blocksBad = 0;         // bursts or groups that did not give a valid message
    // details
    double timeSec = 0, inputRate = 0;
    uint64_t bursts = 0;            // SAME data bursts found
    bool toneNow = false;           // the 1050 Hz tone is on the air now
    double lastToneSec = -1e9;
    int country = 0;                // same::Country the location codes are named by
    std::vector<SameAlert> alerts;  // newest first
    std::vector<SameTone> tones;    // newest first
};

inline std::string sameSummary(const SameTelemetry& t) {
    char b[200];
    for (const SameAlert& a : t.alerts) {
        if (a.eom || !a.valid) continue;
        snprintf(b, sizeof b, "%s (%s), %zu location%s, %llu message%s", a.msg.eventName.empty() ? a.msg.event.c_str() : a.msg.eventName.c_str(), a.msg.event.c_str(), a.msg.locs.size(),
                 a.msg.locs.size() == 1 ? "" : "s", (unsigned long long)t.blocksOk, t.blocksOk == 1 ? "" : "s");
        return b;
    }
    if (t.toneNow) return "1050 Hz alert tone";
    snprintf(b, sizeof b, "listening for SAME alerts, %llu message%s", (unsigned long long)t.blocksOk, t.blocksOk == 1 ? "" : "s");
    return b;
}

} // namespace dect2
