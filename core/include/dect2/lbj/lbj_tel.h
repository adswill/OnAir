// LBJ (Chinese railway train broadcast) telemetry. RxTelemetry::lbj, engine standard 83, active standard 82.
//
// Locomotive-to-ground warning broadcasts on about 821 MHz (FM voice channel, 1200 bit/s FSK payload after
// demodulation). Fields of interest: train number, kilometre marker, locomotive identity, direction and speed.
// Receive only; nothing is transmitted.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct LbjMessage {
    int64_t wallTime = 0;
    double timeSec = 0;
    std::string train;           // full train number (prefix + digits)
    std::string trainBase;       // numeric part
    std::string prefix;          // letter prefix (T, K, ...)
    std::string direction;       // up (even train number) / down (odd)
    std::string speedKmh;
    std::string kmMarker;        // kilometre post (e.g. 0123.4)
    std::string loco;            // locomotive type and number
    std::string locoCode;
    std::string route;
    std::string category;
    std::string locoSerial, cabEnd, unknownTail, networkTime;
    double latitude = 0, longitude = 0;   // WGS-84; only meaningful with positionValid
    bool positionValid = false, paired = false, timeSync = false;
    uint32_t address = 0;
    int correctedBits = 0;
    bool detailed = false;       // long message with locomotive / route
    bool crcOk = false;
    float rssiDb = -200;
    float afcHz = 0;
};

struct LbjTelemetry {
    uint64_t seq = 0;
    int state = 0;                 // 0 no input, 1 listening, 2 message in the last 30 s
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // messages shown / LBJ pages that failed the checks
    uint64_t bursts = 0;                    // bursts with a POCSAG sync word (any address)
    uint64_t emptyFrames = 0;               // short messages with every field blank
    double timeSec = 0;
    double inputRate = 0;
    double centerHz = 0;
    uint64_t blocks = 0;
    std::vector<LbjMessage> messages;
};

constexpr size_t kLbjLogMax = 400;

inline std::string lbjSummary(const LbjTelemetry& t) {
    if (t.state == 0) return "LBJ: waiting for samples";
    if (t.messages.empty()) {
        char b[128];
        snprintf(b, sizeof b, "LBJ: listening (%.0f dBFS)", t.levelDb);
        return b;
    }
    const LbjMessage& m = t.messages.front();
    char b[192];
    if (!m.train.empty() && !m.kmMarker.empty())
        snprintf(b, sizeof b, "LBJ %s at km %s", m.train.c_str(), m.kmMarker.c_str());
    else if (!m.train.empty())
        snprintf(b, sizeof b, "LBJ %s", m.train.c_str());
    else
        snprintf(b, sizeof b, "LBJ: %zu message%s", t.messages.size(), t.messages.size() == 1 ? "" : "s");
    return b;
}

} // namespace dect2
