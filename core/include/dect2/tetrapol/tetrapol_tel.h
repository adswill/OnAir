// TETRAPOL receiver telemetry (PAS 0001-2 / 0001-3-2), shared by the receiver and the UI.
// The cells (D_SYSTEM_INFO) and the event log are saved with the results. The call table (calls, activations) is for the screen only: it is
// never saved, exported or logged, and it holds group and channel numbers only for clear calls.
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
};

// A group call announced on a control channel (D_GROUP_ACTIVATION, screen only). group / channel stay -1 unless the call is clear.
struct TetrapolActivation {
    int64_t lastWall = 0;
    double lastSec = 0;
    int verdict = 0;
    int groupId = -1, channelId = -1;
    uint64_t heard = 0;
};

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
    snprintf(b, sizeof b, "%zu TETRAPOL carrier%s, %zu with system info%s", t.cells.size(), t.cells.size() == 1 ? "" : "s", named,
             t.synced ? "" : " (no frame sync)");
    return b;
}

} // namespace dect2
