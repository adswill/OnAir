// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRAPOL receiver telemetry (PAS 0001-2 / 0001-3-2), shared by the receiver and the UI.
// The cells (D_SYSTEM_INFO) and the event log are saved with the results. The call table, the message list and the neighbour list are for the
// screen only. Signalling is shown as it is sent in the clear (groups, channels, addresses); encrypted voice is never decoded.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct TetrapolCell {
    double freqHz = 0;
    double cfoHz = 0;
    int scramble = -1;
    int country = -1;          // D_SYSTEM_INFO country code when parsed
    int systemId = -1;
    int bnId = -1;
    int cellId = -1;           // BS id of the cell id
    int rswId = -1;
    int locArea = -1;
    int channelId = -1;        // only in the disconnected-mode D_SYSTEM_INFO
    int band = 0;              // 1 VHF, 2 UHF (frame coding found)
    bool hasSysinfo = false;
    float levelDb = -200, snrDb = -99;
    int64_t firstWall = 0, lastWall = 0;
    double lastSec = 0;
    uint64_t framesOk = 0, framesBad = 0;
};

struct TetrapolEvent { int64_t wallTime = 0; double sec = 0; std::string text; };
struct TetrapolCarrierSeen { double freqHz = 0; float levelDb = 0; bool synced = false; };

// A call heard on the traffic channel being received (screen only). verdict: 0 encryption state unknown, 1 clear, 2 encrypted.
struct TetrapolCall {
    int64_t startWall = 0;
    double startSec = 0, lastSec = 0;
    int verdict = 0;
    bool active = false;
    uint64_t voiceFrames = 0, decoded = 0, damaged = 0;
    std::string wavPath;       // the recording of this call (clear calls with recording on)
    std::string talker;        // the talking party (UI_CD TP_ADDRESS / TP_ALIAS on the traffic channel), empty when not sent
    int groupId = -1;          // from the control channel's D_GROUP_ACTIVATION with this channel's scrambling constant, -1 unknown
};

// A group call announced on a control channel (D_GROUP_ACTIVATION, screen only). Signalling is in the clear even for an encrypted call.
struct TetrapolActivation {
    int64_t lastWall = 0;
    double lastSec = 0;
    int verdict = 0;
    int groupId = -1, channelId = -1;
    int dScramble = -1;        // the traffic channel's downlink scrambling constant
    int keyRef = -1;
    int coverage = -1;
    uint64_t heard = 0;
};

// A signalling message heard (screen only): repeats of the same text are counted on one row.
struct TetrapolMsg {
    int64_t lastWall = 0;
    double lastSec = 0;
    std::string channel;       // BCH, SDCH, PCH, RCH (control channel), SCH, VCH (traffic channel)
    std::string text;
    uint64_t count = 0;
};

// An adjacent cell from D_NEIGHBOURING_CELL (screen only)
struct TetrapolNeighbourRow { int channelId = -1, bnNb = -1, bsId = -1, rswId = -1, param = -1; };

struct TetrapolTelemetry {
    uint64_t seq = 0;
    int state = 0;             // 0 no input, 1 searching, 2 a cell in the last 30 s
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // frames that passed / failed their CRC
    double timeSec = 0;
    double inputRate = 0;
    double centerHz = 0;
    uint64_t blocks = 0;
    int spacingHz = 12500;     // 12500 or 10000
    // frame layer
    bool synced = false;
    int polarity = 0;          // 0 not known yet, 1 normal, 2 inverted (found automatically unless set by hand)
    bool polarityAuto = true;
    int band = 0;              // 0 not known yet, 1 VHF, 2 UHF coding
    int scramble = -1;         // the locked scrambling constant
    uint64_t voiceFrames = 0, dataFrames = 0, dataBlocks = 0, tsdus = 0;
    // voice
    uint64_t voicePassed = 0, voiceRefused = 0, voiceDecoded = 0;
    int callVerdict = -1;      // the call on this channel now: -1 none, else TetrapolCall::verdict
    bool codecReady = false;
    std::string voiceStatus;   // what happens to the voice now
    std::string wavPath;       // the call recording being written
    std::vector<TetrapolCarrierSeen> carriers;
    std::vector<TetrapolCell> cells;
    std::vector<TetrapolEvent> events;
    std::vector<TetrapolCall> calls;
    std::vector<TetrapolActivation> activations;
    std::vector<TetrapolMsg> msgs;
    std::vector<TetrapolNeighbourRow> neighbours;
    int channelKind = 0;       // 0 not known yet, 1 control channel (D_SYSTEM_INFO / superframe), 2 traffic channel (voice frames)
    int superframePos = -1;    // frame number in the 200-frame superframe of a control channel, -1 unknown
    uint64_t stuffing = 0;     // fill frames (PAS 0001-3-3 7.4.1.9)
    std::string talker;        // the talking party on this traffic channel now
};

// Frequency of traffic channel `channel` on a network whose control channel is channel `ccChannel` at ccHz: TETRAPOL channels sit on the
// network's raster (10 kHz in France, 12.5 kHz in e.g. Switzerland). The signalling carries channel NUMBERS only, so the control channel's own
// number must be known (the user enters it); a network with a duplex gap or a non-contiguous plan needs a manual correction. < 0 when unknown.
inline double tetrapolChannelHz(double ccHz, int ccChannel, int channel, int spacingHz) {
    if (ccChannel < 0 || channel < 0 || ccHz <= 0) return -1;
    return ccHz + (double)(channel - ccChannel) * (double)spacingHz;
}

constexpr size_t kTetrapolLogMax = 300;
constexpr size_t kTetrapolMax = 200;
constexpr size_t kTetrapolCallsMax = 50;
constexpr size_t kTetrapolMsgsMax = 200;

inline std::string tetrapolSummary(const TetrapolTelemetry& t) {
    if (t.state == 0) return "waiting for samples";
    size_t named = 0;
    for (const auto& c : t.cells) if (c.hasSysinfo) named++;
    char b[200];
    if (t.voiceFrames) {
        snprintf(b, sizeof b, "traffic channel: %llu voice frames, %llu to the codec, %s", (unsigned long long)t.voiceFrames,
                 (unsigned long long)t.voicePassed, t.callVerdict == 1 ? "clear call" : t.callVerdict == 2 ? "encrypted call" : t.callVerdict == 0 ? "call, encryption unknown" : "no call now");
        return b;
    }
    if (t.channelKind == 1) {
        snprintf(b, sizeof b, "control channel: %zu cell%s with system info, %zu group call%s announced, %zu message types", named, named == 1 ? "" : "s",
                 t.activations.size(), t.activations.size() == 1 ? "" : "s", t.msgs.size());
        return b;
    }
    if (t.synced && t.stuffing > 0 && t.tsdus == 0) return "idle carrier: fill frames only";
    snprintf(b, sizeof b, "%zu TETRAPOL carrier%s, %zu with system info%s", t.cells.size(), t.cells.size() == 1 ? "" : "s", named,
             t.synced ? "" : " (no frame sync)");
    return b;
}

} // namespace dect2
