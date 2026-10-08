// Iridium receiver telemetry, shared by the receiver and the UI. Every vector is capped; a report stays well below 100 kB.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// Frame type counters: the index is (int)IridiumType of iridium_frame.h (0 = not recognised).
constexpr int kIridiumTypeSlots = 13;
inline const char* iridiumTypeLabel(int i) {
    static const char* n[kIridiumTypeSlots] = {"other", "IRA", "IBC", "ISY", "ITL", "MSG", "IDA", "IIP", "IIQ", "IIR", "IIU", "IMS", "voice"};
    return i >= 0 && i < kIridiumTypeSlots ? n[i] : "?";
}

struct IridiumSatInfo {
    int id = -1;                     // satellite number as the frames carry it (Iridium's internal number, 0..127)
    uint64_t frames = 0;             // frames heard from it
    double lastHeard = 0;            // receiver time, seconds
    double freqOffsetHz = 0;         // offset of its last burst from the channel grid (Doppler plus the radio's error)
    bool hasPos = false;             // sub-point from a ring alert with a satellite position
    double lat = 0, lon = 0, altKm = 0;
    std::vector<int> beams;          // beam ids heard, ascending (at most 48)
};

struct IridiumRingAlert {            // IRA
    double time = 0;                 // receiver time, seconds
    int sat = -1, beam = -1;
    bool hasPos = false;
    double lat = 0, lon = 0, altKm = 0;   // as the frame carries it: the satellite (hundreds of km) or the beam centre (near 0)
    int paged = 0;                   // TMSIs paged (the identities are not kept)
    double freqHz = 0;
};

struct IridiumIbcInfo {              // IBC
    double time = 0;
    int sat = -1, beam = -1;
    bool hasTime = false;
    double unixTime = 0;             // Iridium time converted to UTC seconds since 1970
};

struct IridiumPagerMsg {             // MSG, assembled over its parts
    double time = 0;
    int ric = -1, seq = -1;
    std::string text;
    bool complete = false;
};

struct IridiumAcarsMsg {             // ACARS carried in short burst data (IDA frames joined), Phase B
    double time = 0;
    bool downlink = true;            // true: from the satellite (to the aircraft)
    char mode = 0, ack = 0, blockId = 0;
    std::string reg, label, seq, flight, text;
    bool more = false;               // another block follows
};

struct IridiumMapPoint {             // positions for the map, from ring alerts
    double time = 0;
    int sat = -1, beam = -1;
    float lat = 0, lon = 0, altKm = 0;
    bool satellite = false;          // true: the satellite's position; false: a beam centre on the ground
};

struct IridiumBurstDot {             // burst scatter: the last 2 s
    float age = 0;                   // seconds before the report (0 .. 2)
    float freqKHz = 0;               // relative to the tuned centre
    uint8_t kind = 0;                // 0 detected, 1 unique word found, 2 frame decoded
};

struct IridiumTelemetry {
    uint64_t seq = 0;            // grows with every report
    int state = 0;               // 0 searching, 1 signal (bursts with a unique word), 2 decoding (frames with good codes)
    double cfoHz = 0;            // offset of the last good burst from the channel grid: satellite Doppler plus the radio's error
    float snrDb = 0;             // mean Es/N0 of the bursts with a unique word over the last report
    bool dataValid = false;      // a frame has been decoded since the start or the last retune
    uint64_t blocksOk = 0;       // frames whose codes checked out
    uint64_t blocksBad = 0;      // bursts with a unique word whose frame failed its checks (voice not counted)

    double timeSec = 0;          // receiver time of this report (seconds of signal since the start or the last retune)
    double inputRate = 0;        // sample rate
    double centerMhz = 0;        // tuned centre the receiver assumes (setCenterMhz)
    // bursts: totals and per second over the last report
    uint64_t bursts = 0, demodulated = 0, uwOk = 0, dropped = 0, downlink = 0, uplink = 0;
    uint64_t duplicates = 0;     // a strong burst found twice (or its spectral skirt): counted once
    float burstsPerSec = 0, demodPerSec = 0, uwPerSec = 0, framesPerSec = 0;
    float confidence = 0;        // mean demodulator confidence (percent of symbols within 22 degrees) over the last report
    uint64_t typeCount[kIridiumTypeSlots] = {};
    uint64_t voiceFrames = 0;    // counted only: voice is never decoded or stored
    uint64_t pagedTotal = 0;     // TMSIs paged in all ring alerts heard
    // time from the broadcast channel
    bool hasTime = false;
    double iridiumUtc = 0;       // UTC seconds since 1970 at timeSec, from the last IBC time
    std::vector<IridiumSatInfo> sats;          // at most 128, by id
    std::vector<IridiumRingAlert> ringAlerts;  // newest last, at most 100
    std::vector<IridiumIbcInfo> ibc;           // newest last, at most 32
    std::vector<IridiumPagerMsg> messages;     // newest last, at most 100 (complete, and incomplete ones that timed out)
    std::vector<IridiumMapPoint> positions;    // newest last, at most 500
    std::vector<IridiumBurstDot> scatter;      // at most 2000
    uint64_t sbdPackets = 0;                   // IDA packets joined (short burst data)
    std::vector<IridiumAcarsMsg> acars;        // newest last, at most 50
};

inline std::string iridiumSummary(const IridiumTelemetry& t) {
    if (t.state == 0) return "Iridium: searching";
    char b[200];
    snprintf(b, sizeof b, "Iridium: %s, %.0f bursts/s, %.0f frames/s, %zu satellites, %llu ring alerts, %zu messages", t.state == 1 ? "signal" : "decoding",
             t.burstsPerSec, t.framesPerSec, t.sats.size(), (unsigned long long)t.typeCount[1], t.messages.size());
    return b;
}

} // namespace dect2
