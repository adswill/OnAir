// EBICab / Eurobalise telemetry, shared by the receiver and the UI. The first members are copied into RxTelemetry (history plot, top bar).
#pragma once
#include "dect2/ebicab/ebicab_proto.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kEbicabTelegramsMax = 200;
constexpr size_t kEbicabBurstsMax = 100;
constexpr size_t kEbicabBalisesMax = 100;

struct EbicabTelegram {
    int64_t utc = 0;
    double timeSec = 0;
    bool longFmt = false, inverted = false;
    ebicab::Header header;
    std::vector<ebicab::Packet> packets;   // SUBSET-026 packets after the header
    std::string hex;             // the user bits
    int repeats = 1;             // copies seen in a row (the balise repeats its telegram while the train passes)
    float snrDb = 0;
};

// A burst of FSK that gave no telegram (another system, a damaged telegram, EBICab)
struct EbicabBurst {
    int64_t utc = 0;
    double timeSec = 0, durationUs = 0;
    int bits = 0;                // bit decisions made in the burst (about 564.48 kbit/s)
    std::string head;            // the first bits as hex (at most 256)
    std::string hint;            // "length of an EBICab 900 telegram (255 bits)" and so on
    float snrDb = 0;
};

struct EbicabBalise {
    int nidC = 0, nidBg = 0, nPig = 0, nTotal = 0;
    int mVersion = 0;
    uint64_t count = 0;
    int64_t firstSeen = 0, lastSeen = 0;
};

struct EbicabTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 nothing, 1 FSK bursts seen, 2 telegrams decoded lately
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // telegrams / bursts without a telegram
    double timeSec = 0, inputRate = 0;
    uint64_t bursts = 0, longTelegrams = 0, shortTelegrams = 0, invertedTelegrams = 0, parityHits = 0;
    double bitRate = 0;          // measured data rate in the last burst (nominal 564 480)
    std::vector<EbicabTelegram> telegrams;   // newest first
    std::vector<EbicabBurst> burstLog;       // newest first
    std::vector<EbicabBalise> balises;       // newest first
};

inline std::string ebicabSummary(const EbicabTelemetry& t) {
    char b[160];
    if (t.blocksOk) { std::snprintf(b, sizeof b, "Eurobalise: %llu telegrams (%llu long, %llu short)", (unsigned long long)t.blocksOk, (unsigned long long)t.longTelegrams, (unsigned long long)t.shortTelegrams); return b; }
    if (t.bursts) { std::snprintf(b, sizeof b, "Balise band: %llu bursts, no telegram decoded", (unsigned long long)t.bursts); return b; }
    return "Balise: no signal";
}

} // namespace dect2
