// HF utility receivers (families 430 .. 439) telemetry, shared by the receiver and the panel. Keep the first members (copied into RxTelemetry).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr int kHfuFamilyFirst = 430, kHfuKinds = 10;
inline const char* hfuName(int k) {
    static const char* n[kHfuKinds] = {"MIL-STD-188-110 serial", "STANAG 4285", "ARQ-E", "ARQ-N", "AUTOSPEC", "SPREAD", "G-TOR", "EFR teleswitch", "Piccolo Mk VI", "Coquelet-8"};
    return k >= 0 && k < kHfuKinds ? n[k] : "HF utility";
}

struct HfUtilEvent {
    int64_t utc = 0;
    std::string text;
};

struct HfUtilTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 searching, 1 signal found, 2 decoding
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;
    int kind = 0;
    std::string mode;            // rate / interleaver, or the found baud and shift
    double baud = 0, centreHz = 0, shiftHz = 0;
    std::string text;            // decoded text, newest last (kept to a few thousand characters)
    std::vector<HfUtilEvent> events;   // newest first
    uint64_t pdus = 0;           // STANAG 5066 D_PDUs seen with a good header CRC
};

inline std::string hfUtilSummary(const HfUtilTelemetry& t) {
    char b[200];
    if (t.state == 0) snprintf(b, sizeof b, "%s: searching", hfuName(t.kind));
    else if (t.mode.empty()) snprintf(b, sizeof b, "%s: signal found", hfuName(t.kind));
    else snprintf(b, sizeof b, "%s: %s", hfuName(t.kind), t.mode.c_str());
    return b;
}

}  // namespace dect2
