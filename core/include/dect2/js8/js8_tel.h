// JS8Call receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One decoded frame
struct Js8Decode {
    double slotUtc = 0;          // start of its slot (UTC seconds; with a given start time, that clock)
    int submode = 0;             // js8::Submode: 0 normal, 1 fast, 2 turbo
    float hz = 0;                // audio frequency of tone 0 (dial + hz)
    float snrDb = 0;             // in 2500 Hz, as JS8Call / WSJT-X
    float dt = 0;                // s, against the nominal start
    std::string chars;           // the 12 characters of the frame (JS8Call's raw text)
    int flags = 0;               // transmission flags: 1 first, 2 last, 4 fast data
    int type = -1;               // js8::FrameType
    std::string text;            // as JS8Call shows the frame
    std::string from, to, grid;
    int pass = 0;                // 0 first pass, 1 after subtracting the stronger signals
};

// A message put together from its frames (one transmission, first to last frame, at one frequency)
struct Js8Message {
    double utc = 0;              // its first frame's slot
    int submode = 0;
    float hz = 0, snrDb = 0;
    std::string from, to, cmd;
    std::string text;            // "LY2ABC: DL1XYZ HELLO FROM VILNIUS"
    int frames = 0;
    bool complete = false;       // the last frame came
    bool relay = false;          // a relay: from -> via (to) -> relayTo
    std::string relayTo;
};

// A station heard
struct Js8Station {
    std::string call, grid;
    double lat = 0, lon = 0;
    bool hasPos = false;
    float snrDb = 0, hz = 0;
    double lastUtc = 0;
    int submode = 0;
    int frames = 0;
    bool cq = false, heartbeat = false;
    std::string lastText;
};

constexpr size_t kJs8Keep = 600;      // decodes kept
constexpr size_t kJs8MsgKeep = 200;   // messages kept

struct Js8Telemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 nothing yet, 1 slots being decoded, 2 frames decoded in the last minute
    double cfoHz = 0;            // not measured (the frequencies are where they are): always 0
    float snrDb = 0;             // of the newest decode
    float levelDb = -200;
    bool dataValid = false;      // a frame in the last minute
    uint64_t blocksOk = 0;       // frames decoded
    uint64_t blocksBad = 0;      // candidates that did not decode
    // details
    double timeSec = 0, inputRate = 0;
    double nowUtc = 0;           // the slot clock of the newest audio
    int timeMode = 0;            // 0 system clock, 1 the given start time
    bool enabled[3] = {true, true, true};
    uint64_t slots[3] = {};
    int lastSlotCount[3] = {};
    float decodeMs[3] = {};
    float clockErr = 0;          // median DT of the recent decodes, s
    float clockTrim = 0;         // the automatic correction in use, s (added to the computer clock)
    std::vector<Js8Decode> decodes;          // oldest first
    std::vector<Js8Message> messages;        // oldest first
    std::vector<Js8Station> stations;        // by last heard, newest first
    std::vector<float> spectrum;             // dB per bin, loHz .. hiHz, for the waterfall
    double loHz = 0, hiHz = 0;
    uint64_t specSeq = 0;
};

inline std::string js8Summary(const Js8Telemetry& t) {
    char b[200];
    if (t.decodes.empty()) snprintf(b, sizeof b, "listening, %llu slots", (unsigned long long)(t.slots[0] + t.slots[1] + t.slots[2]));
    else snprintf(b, sizeof b, "%llu frames, %zu stations; %s", (unsigned long long)t.blocksOk, t.stations.size(), t.decodes.back().text.c_str());
    return b;
}

} // namespace dect2
