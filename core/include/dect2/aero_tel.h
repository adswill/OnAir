// Inmarsat Aero receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

struct AeroChannelInfo {
    double offsetHz = 0;          // carrier relative to the user's frequency (the app adds the tuned frequency)
    int bitRate = 0;              // 600, 1200 or 10500; 0 while the rate is still being tried
    int state = 0;                // 0 carrier seen, 1 carrier locked (loops running), 2 frames in sync, 3 data (SU checks good)
    float ebn0Db = 0;             // per channel bit, from the decisions
    float levelDb = 0;            // carrier peak above the noise floor in the band search
    double cfoHz = 0;             // where the loops put the carrier against the band search's estimate
    uint64_t frames = 0, uwMisses = 0, uwBitErrors = 0;
    uint64_t susOk = 0, susBad = 0;
    float channelBer = 0;         // error rate of the coded bits before the Viterbi decoder (last frame)
    double lastHeard = 0;         // stream time of the last good SU (s)
};

struct AeroSuTypeCount {
    int type = 0;                 // SU type octet
    std::string name;
    uint64_t count = 0;
};

struct AeroLogonEntry {
    double time = 0;              // stream seconds
    int64_t wallTime = 0;         // Unix time when it was decoded
    int channel = 0;              // index into channels at that time
    uint32_t aesId = 0;           // ICAO 24-bit address of the aircraft
    int gesId = -1;
    bool logon = true;            // false: logoff
};

struct AeroMessage {
    double time = 0;
    int64_t wallTime = 0;
    int channel = 0;
    int bitRate = 0;
    uint32_t aesId = 0;
    int gesId = -1;
    bool uplink = true;           // ground to aircraft (the P channel carries uplinks)
    std::string mode, registration, flight, label, labelText, blockId, msgNo, text;
    bool crcOk = false;
    std::string decoded;          // ADS-C groups in plain words (aero_adsc.h), "" when the message has none
    bool hasPos = false;          // the message carried a position (ADS-C or a text report, aero_pos.h)
    double lat = 0, lon = 0;
};

struct AeroTrackPoint {
    double lat = 0, lon = 0;
    int altFt = 0;
    double time = 0;              // stream seconds
};

struct AeroAircraft {
    uint32_t aesId = 0;
    std::string registration, flight, lastLabel, lastText;
    uint64_t messages = 0;
    double lastHeard = 0;
    int64_t lastWall = 0;
    bool loggedOn = false;
    // the last position report (ADS-C or text): they come every few minutes, not every second as on ADS-B
    bool hasPos = false;
    double lat = 0, lon = 0;
    bool hasAlt = false;
    int altFt = 0;
    bool hasTrack = false;        // from the report, else from the last two positions
    double trackDeg = 0;
    bool hasSpeed = false;
    double speedKt = 0;
    int posSource = 0;            // 1 ADS-C, 2 text report
    std::string posKind;          // "ADS-C Basic report", "POS report", ...
    double posTime = 0;           // stream seconds when it was decoded
    int64_t posWall = 0;
    double reportSecPastHour = -1;   // ADS-C time stamp, seconds past the hour
    int reportSecOfDay = -1;         // text reports: their own time, seconds of the UTC day
    uint64_t positions = 0;
    std::vector<AeroTrackPoint> track;                 // oldest first, up to 24, the last is the current position
    std::vector<std::pair<double, double>> route;      // ADS-C predicted route of the last report (next, next + 1)
};

struct AeroTelemetry {
    uint64_t seq = 0;            // grows with every report
    int state = 0;               // 0 searching, 1 signal, 2 decoding
    double cfoHz = 0;            // carrier error: the locked channel nearest the user's frequency, against that frequency
    float snrDb = 0;             // Eb/N0 of that channel (dB, per channel bit)
    bool dataValid = false;      // a message has been decoded
    uint64_t blocksOk = 0;       // SUs with a good check (all channels)
    uint64_t blocksBad = 0;      // the ones that failed it

    double inputRate = 0;
    double timeSec = 0;                          // stream time
    std::vector<AeroChannelInfo> channels;       // up to 6
    std::vector<AeroSuTypeCount> suTypes;        // up to 64
    std::vector<AeroLogonEntry> logons;          // newest last, up to 100
    std::vector<AeroMessage> messages;           // newest last, up to 200
    std::vector<AeroAircraft> aircraft;          // up to 100, most recent first
    uint64_t messagesTotal = 0, logonsTotal = 0, frames = 0;
    uint64_t positionsTotal = 0;                 // messages that carried a position
    bool suLayer = false;                        // the SU layer decodes (false: frames and checks only)
};

inline std::string aeroSummary(const AeroTelemetry& t) {
    if (t.state == 0) return "Inmarsat Aero: searching";
    int locked = 0;
    for (const auto& c : t.channels) locked += c.state >= 2;
    char b[160];
    if (t.state == 1) {
        std::snprintf(b, sizeof b, "Inmarsat Aero: %zu carrier%s, none in sync", t.channels.size(), t.channels.size() == 1 ? "" : "s");
        return b;
    }
    std::snprintf(b, sizeof b, "Inmarsat Aero: %d channel%s, Eb/N0 %.1f dB, SUs %llu ok %llu bad, %llu messages, %zu aircraft", locked, locked == 1 ? "" : "s",
                  t.snrDb, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.messagesTotal, t.aircraft.size());
    return b;
}

} // namespace dect2
