// VDL Mode 3 receiver telemetry, shared by the receiver and the UI.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct Vdl3Burst {
    uint64_t serial = 0;
    double timeSec = 0;
    int64_t wallTime = 0;
    int kind = 0;                 // Vdl3Kind
    std::string kindName;         // "M uplink", "V/D data" ...
    std::string sync;             // "S1", "S1*", "S2", "S2*"
    int symbols = 0;              // after the training sequence
    bool golayOk = false;
    int golayFixed = 0;           // bits the Golay words corrected
    std::string info;             // the Golay words' information bits, 3 hex digits per word (system data, header, handoff check)
    bool rsOk = false;            // V/D data: the Reed-Solomon (72,62) word was good
    int rsFixed = 0;
    std::string dataHex;          // V/D data: the 62 information octets; V/D voice: the 72 octets as received
    std::string dataText;         // V/D data: the printable text, when the octets are text
    float levelDb = -120;
    float snrDb = 0;
    float cfoHz = 0;
    float evmPct = 0;             // phase error of the D8PSK decisions, rms, % of a symbol step
    std::string summary;
};

struct Vdl3Telemetry {
    uint64_t seq = 0;
    int state = 0;                // 0 listening, 1 bursts seen, 2 decoding
    double cfoHz = 0;
    float snrDb = 0;
    bool dataValid = false;
    uint64_t blocksOk = 0;        // bursts whose Golay (and for data, RS) words decoded
    uint64_t blocksBad = 0;
    double centerHz = 0;
    double nowSec = 0;
    double inputRate = 0;
    bool rateOk = true;
    float levelDb = -120;
    float noiseDb = -120;
    uint64_t syncs = 0;           // training sequences found
    uint64_t mUplink = 0, mUplink3T = 0, handoff = 0, pollResp = 0, mDownlink = 0, netEntry = 0, vdData = 0, vdVoice = 0;
    std::vector<Vdl3Burst> bursts;   // newest first
};

constexpr size_t kVdl3BurstMax = 400;

inline std::string vdl3Summary(const Vdl3Telemetry& t) {
    char b[240];
    if (!t.rateOk) {
        snprintf(b, sizeof b, "VDL3: sample rate %.0f kHz is too low, use at least 250 kHz", t.inputRate / 1e3);
        return b;
    }
    if (t.state == 0) return "VDL3: listening for VDL Mode 3 bursts";
    snprintf(b, sizeof b, "VDL3: %llu bursts  %llu bad  M up %llu  M down %llu  data %llu  voice %llu  CFO %+.0f Hz",
             (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)(t.mUplink + t.mUplink3T + t.handoff),
             (unsigned long long)(t.mDownlink + t.netEntry + t.pollResp), (unsigned long long)t.vdData, (unsigned long long)t.vdVoice, t.cfoHz);
    return b;
}

} // namespace dect2
