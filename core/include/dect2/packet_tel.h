// APRS / Packet receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One decoded AX.25 frame
struct PacketFrameInfo {
    double timeSec = 0;          // signal seconds since the start
    std::string from, to, path;  // path: the digipeaters, "WIDE1-1*,WIDE2-1"
    std::string type;            // APRS type ("Position", "Mic-E", ...) or the AX.25 frame type for anything else
    std::string summary;         // what the frame says, in one line
    std::string info;            // the information field as received (control characters kept)
    int baud = 1200;
    bool hasPos = false;
    double lat = 0, lon = 0;
};

// One station heard (by its source callsign, SSID included; objects and items are listed under their own name)
struct PacketStation {
    std::string call;
    char symTable = 0, symCode = 0;
    double lastHeardSec = 0;     // signal seconds
    uint32_t count = 0;          // frames
    bool hasPos = false;
    double lat = 0, lon = 0;
    bool hasAlt = false; double altM = 0;
    bool hasCourse = false; int courseDeg = 0;
    bool hasSpeed = false; double speedKnots = 0;
    std::string comment;
    bool isObject = false;       // an object or item sent by another station
    std::string via;             // the station that sent the object
};

struct PacketTelemetry {
    uint64_t seq = 0;            // grows with every report
    int state = 0;               // 0 no signal, 1 carrier but no good frame lately, 2 decoding frames
    double cfoHz = 0;            // carrier error (average of the last frames)
    float snrDb = 0;             // carrier to noise in the channel
    float levelDb = -200;        // power of the whole input, dBFS (-200 = nothing measured yet)
    bool dataValid = false;      // a frame was decoded lately
    uint64_t blocksOk = 0;       // good and bad frames, both speeds together
    uint64_t blocksBad = 0;
    // set-up
    double timeSec = 0;          // signal seconds since the start (now)
    double inputRate = 0;
    // per speed
    uint64_t ok1200 = 0, bad1200 = 0, ok9600 = 0, bad9600 = 0;
    double last1200Sec = -1e9, last9600Sec = -1e9, lastAprsSec = -1e9;
    uint64_t aprsFrames = 0;
    bool carrier = false;
    // results (the newest frame last; the stations newest first)
    std::vector<PacketFrameInfo> frames;
    std::vector<PacketStation> stations;
};

inline std::string packetSummary(const PacketTelemetry& t) {
    char b[160];
    snprintf(b, sizeof b, "APRS / Packet: %llu frames (1200: %llu, 9600: %llu), %zu stations, %llu failed the check, %.1f dBFS",
             (unsigned long long)t.blocksOk, (unsigned long long)t.ok1200, (unsigned long long)t.ok9600, t.stations.size(), (unsigned long long)t.blocksBad, t.levelDb);
    return b;
}

} // namespace dect2
