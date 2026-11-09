// Telemetry of the radio-control and mesh link monitor, shared by the receiver and the UI.
#pragma once
#include "dect2/rclink/rc_proto.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kRcFramesMax = 300;
constexpr size_t kRcDevicesMax = 100;

struct RcFrame {
    int64_t utc = 0;
    double timeSec = 0;
    int proto = 0;
    std::string id, type, detail, hex;
    double freqHz = 0;       // absolute when the radio's centre is known, else relative to the capture's middle
    float snrDb = 0;
    bool encrypted = false;
};

struct RcDevice {
    int proto = 0;
    std::string id, lastType, lastDetail;
    uint64_t count = 0;
    int64_t firstSeen = 0, lastSeen = 0;
    bool hasPos = false;
    double lat = 0, lon = 0, altM = 0;
};

struct RcTelemetry {
    uint64_t seq = 0;
    int state = 0;           // 0 nothing, 1 bursts or preambles seen, 2 frames decoded lately
    double cfoHz = 0;
    float snrDb = 0, levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // frames decoded / bursts or preambles without a frame
    double timeSec = 0, inputRate = 0;
    int protocols = 0;
    uint64_t bursts = 0, loraPreambles = 0;
    bool phraseSet = false;
    std::vector<RcFrame> frames;      // newest first
    std::vector<RcDevice> devices;    // newest first
};

inline std::string rcSummary(const RcTelemetry& t) {
    char b[160];
    if (t.blocksOk) { std::snprintf(b, sizeof b, "%llu frames from %zu link(s)", (unsigned long long)t.blocksOk, t.devices.size()); return b; }
    if (t.bursts || t.loraPreambles) { std::snprintf(b, sizeof b, "%llu bursts and %llu LoRa preambles, no frame decoded yet", (unsigned long long)t.bursts, (unsigned long long)t.loraPreambles); return b; }
    return "No signal";
}

} // namespace dect2
