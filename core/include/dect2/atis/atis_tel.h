// ATIS telemetry. RxTelemetry::atis, engine standard 213, active standard 212 (family 211).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kAtisMessagesMax = 300;
constexpr size_t kAtisStationsMax = 200;

// One ATIS burst
struct AtisMessage {
    uint64_t serial = 0;
    int64_t utc = 0;
    double timeSec = 0;
    std::string digits;          // ten digits, '~' unreadable
    std::string mmsi;            // the nine digits after the 9
    std::string country;         // from the MID
    std::string callsign;        // "PC8075", "?C8075" when the first letter is not known
    std::string name;            // from the ship name table (the MMSI form of the number), "" when unknown
    bool checkOk = false;        // the check symbol matched (after the repair)
    bool standardZ = true;       // starts with 9
    int repaired = 0;            // symbols taken from the second copy or rebuilt
    int erasures = 0;
    bool inverted = false;       // the tones came the other way round (sideband swap in the radio)
    float levelDb = -200;        // channel power during the burst, dB relative to full scale
    double cfoHz = 0;            // carrier-offset branch that decoded it
};

// One identity heard, however many times
struct AtisStation {
    std::string digits, mmsi, country, callsign, name;
    uint64_t count = 0;
    int64_t firstSeen = 0, lastSeen = 0;
    float bestLevelDb = -200;
};

struct AtisTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 nothing, 1 tones seen, 2 a message decoded lately
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // messages with a good / failed check
    double timeSec = 0, inputRate = 0;
    uint64_t otherCalls = 0;     // DSC frames with another format specifier (not ATIS)
    bool listening = false;
    std::vector<AtisMessage> messages;     // newest first
    std::vector<AtisStation> stations;     // newest first
};

inline std::string atisSummary(const AtisTelemetry& t) {
    char b[200];
    if (t.blocksOk) {
        const AtisMessage* m = nullptr;
        for (const auto& x : t.messages) if (x.checkOk) { m = &x; break; }
        std::snprintf(b, sizeof b, "ATIS: %llu messages from %zu station(s)%s%s", (unsigned long long)t.blocksOk, t.stations.size(), m ? ", last " : "", m ? m->digits.c_str() : "");
        return b;
    }
    if (t.blocksBad) { std::snprintf(b, sizeof b, "ATIS: %llu burst(s) with a failed check", (unsigned long long)t.blocksBad); return b; }
    return t.state ? "ATIS: waiting for a message" : "ATIS: no signal";
}

} // namespace dect2
