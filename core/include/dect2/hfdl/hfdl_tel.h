// HFDL (HF Data Link, ARINC 635 / ICAO Doc 9741) receiver telemetry, shared by the receiver and the UI.
// The first members of HfdlTelemetry are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One decoded PDU part: a squitter (SPDU), or one LPDU of an MPDU, as far as OnAir reads it.
struct HfdlMessage {
    uint64_t serial = 0;         // grows by one per message, never restarts
    double timeSec = 0;          // signal seconds at the start of the burst
    int64_t wallTime = 0;        // wall clock (Unix seconds) when it was decoded
    double freqHz = 0;           // the channel (dial frequency; the carrier is 1440 Hz above)
    float levelDb = -120;        // burst power, dBFS
    float snrDb = 0;             // from the equaliser's error on the known symbols
    float cfoHz = 0;             // carrier offset of the burst from the channel
    int bps = 0;                 // 300, 600, 1200 or 1800
    int slots = 1;               // 1 or 2 (the interleaver: 1.8 s or 4.2 s)
    bool squitter = false;       // SPDU
    bool uplink = false;         // ground to air
    int gsId = 0;                // ground station id
    int acId = -1;               // the aircraft id the ground station assigned (-1 none)
    uint32_t icao = 0;           // 24-bit address when the PDU names it (logon) or the logon table knows it
    std::string kind;            // "SPDU", "Logon request", "Unnumbered data", ...
    std::string hfnpdu;          // "Performance data", "Frequency data", "ACARS", "System table" ... ("" none)
    std::string summary;         // one line for the list
    std::string detail;          // the decoded layers, several lines
    // squitter
    int frameIndex = -1;
    bool utcSync = false;
    std::vector<int> activeKhz;  // the frequencies the squitter says are in use (from the built-in station table)
    // ACARS
    bool acars = false;
    std::string reg, flight, label, labelText, msgNum, acarsText;
    char blockId = 0;
    bool acarsCrcOk = false;
    // a position (performance / frequency data, or an ACARS position report)
    bool hasPos = false;
    double lat = 0, lon = 0;
    bool hasAlt = false;
    int altFt = 0;
    std::string posKind;
    int utcSec = -1;             // seconds of the day the aircraft gave (performance / frequency data)
};

struct HfdlTrackPoint { double lat = 0, lon = 0; double time = 0; };

struct HfdlAircraft {
    uint32_t key = 0;            // the ICAO address, or 0x1000000 | gs << 8 | aircraft id while the address is not known
    uint32_t icao = 0;
    int acId = -1, gsId = 0;
    std::string reg, flight;
    uint32_t messages = 0;
    double freqHz = 0;
    double lastHeardSec = 0;
    float levelDb = -120;
    bool loggedOn = false;
    bool hasPos = false;
    double lat = 0, lon = 0;
    bool hasAlt = false;
    int altFt = 0;
    std::string posKind;
    double posTime = 0;
    std::vector<HfdlTrackPoint> track;   // oldest first, at most 24
};

struct HfdlGroundStation {
    int id = 0;
    std::string name;            // from the built-in table ("" not known)
    bool hasPos = false;
    double lat = 0, lon = 0;
    uint32_t squitters = 0, messages = 0;
    double freqHz = 0;           // channel it was last heard on
    double lastHeardSec = 0;     // 0: only named by another station's squitter
    float levelDb = -120;
    bool utcSync = false;
    int frameIndex = -1;
    std::vector<int> activeKhz;  // in use, as the last squitter said
    bool heardDirect = false;
};

struct HfdlChannelInfo {
    double freqHz = 0;           // dial frequency
    int gsId = 0;                // the station of the table on this frequency (0 not known)
    bool inBand = false;
    bool active = false;         // a burst in the last 40 s
    float levelDb = -120;
    float noiseDb = -120;
    float snrDb = 0;
    uint64_t bursts = 0;         // preambles found
    uint64_t good = 0;           // bursts with a good check sequence
    uint64_t bad = 0;
    double lastHeardSec = 0;
    float lastCfoHz = 0;
    int lastBps = 0;
};

struct HfdlTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 searching, 1 preambles, 2 decoding (a good PDU in the last 120 s)
    double cfoHz = 0;            // of the last good burst
    float snrDb = 0;
    bool dataValid = false;
    uint64_t blocksOk = 0;       // PDUs with a good check sequence
    uint64_t blocksBad = 0;      // bursts that failed
    // ---- the rest is for the HFDL screens
    double centerHz = 0;
    double nowSec = 0;
    double inputRate = 0;
    uint64_t squitters = 0, lpdus = 0, acarsMsgs = 0, positionsTotal = 0;
    uint64_t byRate[4] = {};     // good bursts at 300, 600, 1200, 1800 bit/s
    // why a burst did not give a PDU ("why isn't it decoding"), totals over the channels
    uint64_t whyPreambleRejected = 0;   // the coarse search fired, the fine check (frequency, A1+A2 coherence, M1) said no preamble
    uint64_t whyShort = 0;              // preamble ok, the decoder gave fewer than 4 octets
    uint64_t whySpduFcs = 0;            // a squitter (SPDU) whose check sequence failed: the FEC did not repair the burst
    uint64_t whyMpduFcs = 0;            // a data PDU whose header check sequence failed
    uint64_t whyLpduFcs = 0;            // an LPDU inside a good MPDU with a bad check sequence
    uint64_t whyUnknownLpdu = 0;        // an LPDU with a good check but a type OnAir does not decode
    std::vector<HfdlMessage> messages;          // newest first, at most kHfdlMsgMax
    std::vector<HfdlAircraft> aircraft;         // most recently heard first, at most 200
    std::vector<HfdlGroundStation> stations;    // by id
    std::vector<HfdlChannelInfo> channels;      // by frequency
};

constexpr size_t kHfdlMsgMax = 2000;

inline std::string hfdlSummary(const HfdlTelemetry& t) {
    int inBand = 0, act = 0;
    for (const auto& c : t.channels) { inBand += c.inBand; act += c.active; }
    char b[200];
    if (!t.channels.empty() && !inBand) return "HFDL: no channel inside the band, tune to an HFDL band (8.9, 10.06, 11.28, 13.3, 17.94, 21.96 MHz)";
    if (t.state == 0) { snprintf(b, sizeof b, "HFDL: listening to %d channel%s", inBand, inBand == 1 ? "" : "s"); return b; }
    int heard = 0;
    for (const auto& g : t.stations) heard += g.heardDirect;
    snprintf(b, sizeof b, "HFDL: %s  %d of %d channels active  %llu PDUs  %llu bad  %d ground stations  %zu aircraft", t.state == 2 ? "decoding" : "preambles",
             act, inBand, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, heard, t.aircraft.size());
    return b;
}

} // namespace dect2
