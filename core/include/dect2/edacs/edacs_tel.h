// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// EDACS control channel telemetry. RxTelemetry::edacs, engine standard 132, active standard 131 (family 130).
// Control data, the call and talkgroup tables built from it (published layout: edacs_msg.h), the voice channel activity of a frequency table and the
// following of one analog voice channel. ProVoice goes to the user-installed IMBE plug-in; nothing encrypted is handled. Receive only.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct EdacsFrame {
    int64_t wallTime = 0;
    double timeSec = 0;
    uint32_t msgA = 0, msgB = 0;      // the 28 data bits of the two messages of the frame, after majority voting over the three copies
    std::string text;                 // what the frame means (edacs_msg.h)
    int errA = 0, errB = 0;           // bits (of 40) on which the three copies of each message disagreed
    bool good = false;
    int carrierId = 0;          // whole-span decoding: 1 when the row came from a carrier of the span (carrierHz: its offset from the centre)
    double carrierHz = 0;
};

struct EdacsWordCount {
    uint32_t word = 0;                // 28 data bits
    uint64_t count = 0;
};

// A call on a voice channel: announced by a control channel assignment, or only seen as a carrier on the channel
struct EdacsCall {
    int lcn = 0;                      // logical channel number (1 = first entry of the frequency table)
    int gid = -1, lid = -1;           // group (or called unit) and calling unit; -1 not known
    bool group = true, emergency = false, digital = false;
    bool fromActivity = false;        // seen as a carrier only, no assignment heard
    bool active = false;
    bool followed = false;            // this one is the voice channel being listened to
    int64_t wallTime = 0;
    double startSec = 0, lastSec = 0, durSec = 0;
    double freqHz = 0;                // from the frequency table, 0 when the LCN is not in it
    int voiceSlot = -1;               // single-radio trunking: the voice slot decoding it at the same time as the others (0 ...), -1 none
};

struct EdacsTalkgroup {
    int gid = 0;
    uint32_t calls = 0;
    int lastLid = -1, lastLcn = 0;
    int64_t lastWall = 0;
    double lastFreqHz = 0;
    bool active = false;
};

struct EdacsChannelActivity {
    int lcn = 0;
    double freqHz = 0;
    float levelDb = -200;             // power in the channel, dBFS
    float floorDb = -200;             // the quietest it has been lately
    bool active = false;              // a carrier well above that floor
    bool inCapture = true;            // false: outside the captured span
};

struct EdacsTelemetry {
    uint64_t seq = 0;
    int state = 0;                    // 0 no input, 1 listening, 2 a good frame in the last 5 s
    bool locked = false;
    double cfoHz = 0;              // not measured (0)
    float snrDb = 0, levelDb = -200, deviationHz = 0;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // frames whose first message passes the BCH check / frames framed by the sync word that do not
    uint64_t syncs = 0;                     // dotting patterns found
    bool inverted = false;                  // the data polarity found from the sync word and the check bits
    double timeSec = 0, inputRate = 0, centerHz = 0;
    std::vector<EdacsFrame> frames;         // newest first
    std::vector<EdacsWordCount> words;      // the most frequent data words, descending
    // message layer and voice channels
    uint64_t messages = 0, messagesKnown = 0;   // words seen, and of them the ones the layout gave a meaning
    bool siteValid = false;
    int siteId = -1, systemId = -1 /* area (EA only) */, controlLcn = -1, nChannels = -1 /* highest LCN seen */;
    std::vector<EdacsCall> calls;           // active ones first, then the finished, newest first
    std::vector<EdacsTalkgroup> talkgroups; // most recently active first
    std::vector<EdacsChannelActivity> channels;   // one per entry of the frequency table
    size_t lcnTableSize = 0;
    bool follow = false, followActivity = false;
    int followLcn = 0;                      // the voice channel listened to now (0 none)
    bool audioOpen = false;                 // sound is playing from it
    std::string note;                       // why nothing is followed, when it is not (no table, outside the capture ...)
    // single-radio trunking (docs/TRUNK1-3.9.md): every analog call inside the capture is decoded at once, each in its own voice slot
    int voiceSlots = 4;
    int slotsInUse = 0;
    std::string slotsStatus;                // what the voice slots do, or the newest notice (a call outside the capture with the bandwidth it needs)
};

constexpr size_t kEdacsCallMax = 200, kEdacsTgMax = 200;

constexpr size_t kEdacsLogMax = 400;

inline std::string edacsSummary(const EdacsTelemetry& t) {
    char b[160];
    if (t.state == 0) return "EDACS: waiting for samples";
    if (!t.dataValid) { snprintf(b, sizeof b, "EDACS: listening (%.0f dBFS)", t.levelDb); return b; }
    int active = 0;
    for (const auto& c : t.calls) active += c.active;
    if (active) snprintf(b, sizeof b, "EDACS: %s, %d call%s on air%s", t.locked ? "locked" : "no lock", active, active == 1 ? "" : "s", t.followLcn ? ", listening" : "");
    else snprintf(b, sizeof b, "EDACS: %s, %llu good frames", t.locked ? "locked" : "no lock", (unsigned long long)t.blocksOk);
    return b;
}

} // namespace dect2
