// PACTOR-I monitor telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kPactorFramesMax = 300;
constexpr size_t kPactorStationsMax = 60;
constexpr size_t kPactorTextMax = 6000;
constexpr size_t kPactorHist = 120;

struct PactorFrameLog {
    int64_t utc = 0;
    double timeSec = 0;          // signal seconds
    std::string kind;            // "data", "connect", "QRT", "CS1" .. "CS4", "bad packet"
    int baud = 0;                // 100 or 200 (control signals: 100)
    int header = -1;             // 0x55 / 0xAA
    int count = -1;              // packet number 0 .. 3
    bool huffman = false, breakIn = false, qrt = false;
    bool crcOk = false;
    bool memArq = false;         // read only after combining with an earlier copy
    bool repeat = false;         // the same packet again (ARQ repeat or unproto / FEC repeat)
    bool fec = false;            // seen without a control signal after it
    bool inverted = false;       // this cycle used the swapped tone polarity
    int sender = 1;              // 1 or 2: which side of the link (changes at every changeover)
    std::string text, hex, call;
    float snrDb = 0;
    double toneHz = 0;           // lower tone (the pair is 200 Hz apart)
};

struct PactorStation {
    std::string call;
    uint64_t count = 0;
    int64_t firstSeen = 0, lastSeen = 0;
    std::string how;             // "called in a connect packet" / "named in text"
};

struct PactorTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 nothing, 1 tone pair or burst seen, 2 packets decoded lately
    double cfoHz = 0;            // centre of the tone pair relative to 1500 Hz
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0;       // packets with a good CRC
    uint64_t blocksBad = 0;      // packets whose CRC failed
    // details
    double timeSec = 0, inputRate = 0;
    double pairLowHz = 0;        // the locked tone pair (0: none)
    uint64_t csCount = 0, memArqSaves = 0, repeats = 0, fastPackets = 0, slowPackets = 0, changeovers = 0, connects = 0;
    bool fecMode = false;        // packets without control signals
    bool hasFullHuffman = false;
    std::vector<PactorFrameLog> frames;   // newest first
    std::vector<PactorStation> stations;
    std::string text;
    std::vector<float> levelHist;
};

inline std::string pactorSummary(const PactorTelemetry& t) {
    char b[160];
    if (t.blocksOk) {
        snprintf(b, sizeof b, "PACTOR-I: %llu packets, %s", (unsigned long long)t.blocksOk, t.fecMode ? "unproto / FEC" : "ARQ");
        return b;
    }
    if (t.pairLowHz > 0) return "PACTOR-I: tone pair found, no packet yet";
    return "PACTOR-I: no signal";
}

} // namespace dect2
