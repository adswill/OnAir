// Telemetry of the channelised analog voice modes (RxTelemetry::chanvoice, engine standard family + 2, active standard family + 1).
#pragma once
#include "dect2/chanvoice/chv_plan.h"
#include "dect2/chanvoice/chv_subtone.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// What one carrier receiver reports (kept small: it is copied at every update)
struct ChvCarrierTel {
    uint64_t seq = 0;
    int state = 0;                    // 0 listening, 2 squelch open
    double cfoHz = 0;
    float snrDb = 0;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;
    bool open = false;
    double levelDb = -200;
    double quietDb = -30;
    int demod = 0;                    // chv::Demod in use now
    chv::SubTone tone;
};

// One row of the channel list: a channel of the plan, or a carrier outside the plan (key >= the plan size)
struct ChvChannelRow {
    int key = -1;                     // index in chv::channels(service, region); a larger number: a carrier off the plan
    int number = 0;
    std::string label;
    std::string name;
    double mhz = 0;
    double txMhz = 0;
    bool active = false;              // a carrier is on the channel now
    bool open = false;                // and the squelch is open
    bool playing = false;
    bool inScan = false;
    bool onAir = true;                // HF: the schedule says it is broadcasting now
    double levelDb = -200;
    int demod = 0;                    // chv::Demod detected (Auto channels), else the plan's
    chv::SubTone tone;                // the sub-audible tone heard now
    chv::SubTone lastTone;            // the last one heard on this channel
    int calls = 0;
    double totalSec = 0;
    double lastHeardAgoSec = -1;      // -1 never
    int64_t lastHeardWall = 0;
    double peakDb = -200;
};

struct ChvCall {
    uint64_t serial = 0;
    int key = -1;
    int number = 0;
    std::string label;
    double mhz = 0;
    int64_t wallTime = 0;
    double startSec = 0, durationSec = 0;
    double peakDb = -200, meanDb = -200;
    int demod = 0;
    chv::SubTone tone;
    std::string file;                 // the recording ("" when none)
};

struct ChvMdcEvent {
    uint64_t serial = 0;
    int64_t wallTime = 0;
    int key = -1;
    std::string label;
    double mhz = 0;
    uint8_t op = 0, arg = 0;
    uint16_t unitId = 0;
    bool dbl = false;
    uint8_t extra[4] = {0, 0, 0, 0};
    std::string text;
};

// A wireless microphone candidate (the Mic service)
struct ChvMicRow {
    int id = 0;
    double mhz = 0;
    double widthKhz = 0;              // occupied width measured
    double levelDb = -200;
    double snrDb = 0;
    double devKhz = 0;                // peak FM deviation of the audio (0: not measured yet)
    bool fmLike = false;              // constant envelope (analogue FM); false: a wideband digital signal or something else
    bool active = false;
    bool listening = false;
    double ageSec = 0;
    std::string band;
};

struct ChvTelemetry {
    uint64_t seq = 0;
    int state = 0;                    // 0 no input, 1 listening, 2 a channel is open
    double cfoHz = 0;
    float snrDb = 0;
    bool dataValid = false;           // at least one transmission heard
    uint64_t blocksOk = 0, blocksBad = 0;
    double timeSec = 0, inputRate = 0, centerHz = 0;
    int service = 0;                  // chv::Service
    int region = 0;
    bool spanCovers = false;          // the capture is wider than one channel
    int carriers = 0;
    int playingKey = -1;
    std::string playingLabel;
    double playingMhz = 0;
    int scanState = 0;                // marinevhf::ChannelScan::State
    bool scanOn = false;
    bool singleTuner = false;         // HF: the scan asks for a retune (tuneRequestMhz)
    double tuneRequestMhz = 0;        // single tuner: the frequency the scan wants the radio on (0: none)
    int recordedFiles = 0;
    uint64_t callsTotal = 0;
    uint64_t mdcTotal = 0;
    std::vector<ChvChannelRow> rows;  // the plan in order, then carriers off the plan
    std::vector<ChvCall> calls;       // newest first
    std::vector<ChvMdcEvent> mdc;     // newest first
    std::vector<ChvMicRow> mics;
};

constexpr size_t kChvCallMax = 600;
constexpr size_t kChvMdcMax = 400;

inline std::string chvSummary(const ChvTelemetry& t) {
    char b[200];
    if (t.state == 0) return std::string(chv::serviceName((chv::Service)t.service)) + ": waiting for samples";
    int act = 0;
    for (const auto& r : t.rows) if (r.active) act++;
    if ((chv::Service)t.service == chv::Service::Mic) { snprintf(b, sizeof b, "Wireless mic finder: %zu candidates", t.mics.size()); return b; }
    if (!t.playingLabel.empty()) snprintf(b, sizeof b, "%s: listening to %s (%.4f MHz), %d active, %llu calls", chv::serviceName((chv::Service)t.service), t.playingLabel.c_str(), t.playingMhz, act, (unsigned long long)t.callsTotal);
    else snprintf(b, sizeof b, "%s: %d channels active, %llu calls", chv::serviceName((chv::Service)t.service), act, (unsigned long long)t.callsTotal);
    return b;
}

} // namespace dect2
