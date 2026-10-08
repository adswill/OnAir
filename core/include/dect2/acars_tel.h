// ACARS receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

// One decoded block. A value that the block does not carry is empty (strings) or 0 (chars).
struct AcarsMessage {
    uint64_t serial = 0;         // grows by one per message, never restarts: a list row keeps its identity
    double timeSec = 0;          // seconds of signal time at the end of the block
    int64_t wallTime = 0;        // wall clock (seconds since 1970) when it was decoded
    double freqHz = 0;           // channel frequency
    float levelDb = -120;        // carrier level during the block, dB below full scale
    char mode = 0;               // mode character, usually '2'
    std::string reg;             // address (aircraft registration, or the ground address), leading dots removed
    char ack = 0;                // technical ack: a block id, '!' for NAK (nothing acknowledged), '^' for ACK
    std::string label;           // two characters; the DEL of "_<DEL>" shows as 'd'
    std::string labelText;       // what the label means, empty when not known
    char blockId = 0;            // '0'..'9' downlink, letters uplink
    bool downlink = false;       // aircraft to ground (block id is a digit)
    bool finalBlock = true;      // ETX (last block of the message); false: ETB, more blocks follow
    std::string msgNum;          // downlink: message number "M12"
    char msgSeq = 0;             // downlink: block sequence letter of that message
    std::string flightId;        // downlink: six characters, e.g. "EK0201"
    std::string sublabel, mfi;   // H1 messages: "#M1B" and "/AT " style prefixes
    std::string text;            // printable text, up to 220 characters; line breaks are '\n'
    std::string decoded;         // short reading of the text when the format is a known one (OOOI times and airports), else empty
    std::string adsc;            // ADS-C groups in plain words (aero_adsc.h), "" when the text has none
    bool hasPos = false;         // the block carried a position (ADS-C or a text report, aero_pos.h): downlinks only
    double lat = 0, lon = 0;
    bool crcOk = true;           // the block check sequence matched (blocks that fail are counted, not listed)
    int parityFixed = 0;         // bits flipped by the parity and check-sequence repair
};

struct AcarsTrackPoint {
    double lat = 0, lon = 0;
    int altFt = 0;
    double time = 0;             // signal seconds
};

struct AcarsAircraft {
    std::string reg;             // registration as sent
    std::string flight;          // last flight id from a downlink (or from an ADS-C flight id group)
    uint32_t icao = 0;           // 24-bit address from an ADS-C airframe id group, 0 when not known
    std::string lastLabel;       // label of the last block
    uint32_t messages = 0;
    double freqHz = 0;           // channel of the last block
    double lastHeardSec = 0;     // signal time of the last block
    float levelDb = -120;        // level of the last downlink
    // the last position report (ADS-C or text): they come every few minutes, not every second as on ADS-B
    bool hasPos = false;
    double lat = 0, lon = 0;
    bool hasAlt = false;
    int altFt = 0;
    bool hasTrack = false;       // from the report, else from the last two positions
    double trackDeg = 0;
    bool hasSpeed = false;
    double speedKt = 0;
    int posSource = 0;           // 1 ADS-C, 2 text report
    std::string posKind;         // "ADS-C Basic report", "POS report", ...
    double posTime = 0;          // signal seconds when it was decoded
    int64_t posWall = 0;
    double reportSecPastHour = -1;   // ADS-C time stamp, seconds past the hour
    int reportSecOfDay = -1;         // text reports: their own time, seconds of the UTC day
    uint32_t positions = 0;
    std::vector<AcarsTrackPoint> track;                // oldest first, up to 24, the last is the current position
    std::vector<std::pair<double, double>> route;      // ADS-C predicted route of the last report (next, next + 1)
};

struct AcarsChannelInfo {
    double freqHz = 0;
    float levelDb = -120;        // carrier level now (dB below full scale), -120 when nothing is there
    float snrDb = 0;             // carrier over the noise floor in 7 kHz
    float cfoHz = 0;             // carrier offset from the channel centre
    bool active = false;         // a carrier is present: the channel is being decoded
    uint32_t messages = 0;       // good blocks
    uint32_t bad = 0;            // blocks that failed
    double lastHeardSec = 0;     // signal time of the last good block (0 = none yet)
};

struct AcarsTelemetry {
    uint64_t seq = 0;            // grows with every report
    int state = 0;               // 0 searching, 1 signal (a carrier is being decoded), 2 decoding (a message arrived in the last 30 s)
    double cfoHz = 0;            // carrier error of the strongest active channel
    float snrDb = 0;             // signal to noise ratio of the strongest active channel
    bool dataValid = false;      // a message has been decoded
    uint64_t blocksOk = 0;       // frames or messages with a good check
    uint64_t blocksBad = 0;      // the ones that failed it
    // ---- the rest is for the ACARS screens
    double centerHz = 131.5e6;   // what the radio is tuned to (the channel list is relative to it)
    double nowSec = 0;           // signal time of this report
    uint64_t parityFixed = 0;    // blocks that needed bit repair to pass
    uint64_t framesStarted = 0;  // SYN SYN SOH seen
    uint64_t positionsTotal = 0; // blocks that carried a position
    std::vector<AcarsMessage> messages;    // newest first, at most 200
    std::vector<AcarsAircraft> aircraft;   // most recently heard first, at most 100
    std::vector<AcarsChannelInfo> channels;// every channel that has had a carrier (and the active ones), sorted by frequency, at most 24
};

inline std::string acarsSummary(const AcarsTelemetry& t) {
    char b[160];
    int act = 0;
    for (const auto& c : t.channels) act += c.active ? 1 : 0;
    if (t.state == 0) return "ACARS: searching";
    snprintf(b, sizeof b, "ACARS: %s  %d channel%s  %llu ok  %llu bad  %zu aircraft", t.state == 2 ? "decoding" : "signal", act, act == 1 ? "" : "s",
             (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.aircraft.size());
    return b;
}

} // namespace dect2
