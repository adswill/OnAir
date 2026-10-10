// Airband receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One channel of the user's list, as the receiver sees it now
struct AirbandChannelState {
    double freqHz = 0;           // the carrier frequency (an 8.33 channel's real frequency, e.g. 118.008333 MHz for "118.010")
    bool is833 = false;          // 8.33 kHz channel (narrow filter) or 25 kHz channel
    std::string name;            // the channel name, as dialled: "118.010", "118.700"
    std::string label;           // the user's tag: "Tower", "ATIS"
    bool inBand = false;         // inside the sample band (otherwise not received)
    bool open = false;           // squelch open (a transmission, or its hang time)
    bool playing = false;        // its audio goes to the output now (mute, solo, priority and scan applied)
    bool heterodyne = false;     // a second carrier on the channel (two stations at once)
    bool muted = false, solo = false, priority = false;
    float levelDb = -200;        // power in the channel filter, dBFS
    float snrDb = -99;           // carrier to noise in the channel (6.8 kHz for 8.33, 10 kHz for 25 kHz channels), dB
    float carrierHz = 0;         // carrier offset from the channel frequency as measured (the radio's tuning error included)
    uint64_t transmissions = 0;  // transmissions heard since the start
    double lastSec = -1;         // signal time of the last transmission's end (or now while open); -1 = none
};

// One transmission, for the activity log
struct AirbandActivity {
    int64_t wallTime = 0;        // Unix time of its start
    double startSec = 0;         // signal seconds since the start, when the carrier came up
    double durSec = 0;           // how long the carrier was there (hang time not counted; 0 while still on)
    int chan = -1;               // index in the channel list at the time
    std::string name, label;
    double freqHz = 0;
    float snrDb = 0;             // strongest carrier to noise during it
    float levelDb = -200;        // mean channel power during it, dBFS
    bool heterodyne = false;
};

struct AirbandTelemetry {
    uint64_t seq = 0;            // grows with every report
    int state = 0;               // 0 no channel in the band, 1 listening, 2 a channel is open
    double cfoHz = 0;            // the radio's tuning error as measured from the carriers (applied to every channel)
    float snrDb = 0;             // best carrier to noise of the open channels (or of all channels when none is open)
    float levelDb = -200;        // power of the whole input, dBFS (-200 = nothing measured yet)
    bool dataValid = false;      // a channel is open
    uint64_t blocksOk = 0;       // transmissions heard
    uint64_t blocksBad = 0;      // always 0
    // set-up
    double timeSec = 0;          // signal seconds since the start (now)
    double inputRate = 0;
    double centerHz = 0;         // the radio's centre frequency the channels are placed from
    double channelRate = 0;      // the rate each channel is demodulated at
    bool tuneKnown = false;      // cfoHz comes from carriers (not the starting guess)
    float squelchDb = 0;         // the squelch threshold (carrier to noise)
    float hangSec = 0;
    bool scan = false;           // scan: one channel at a time, stops on activity
    int scanChan = -1;           // the channel the scan stopped on (-1 = scanning)
    std::vector<AirbandChannelState> channels;
    std::vector<AirbandActivity> activity;   // newest first, at most kAirbandLogMax
};

constexpr size_t kAirbandLogMax = 300;

inline std::string airbandSummary(const AirbandTelemetry& t) {
    int inBand = 0;
    std::string open;
    for (const auto& c : t.channels) {
        inBand += c.inBand;
        if (c.open) { if (!open.empty()) open += ", "; open += c.label.empty() ? c.name : c.label; }
    }
    char b[160];
    if (t.channels.empty()) return "no channel in the list";
    if (!inBand) return "no channel inside the band: tune nearer";
    if (!open.empty()) snprintf(b, sizeof b, "%s open, %llu transmissions", open.c_str(), (unsigned long long)t.blocksOk);
    else snprintf(b, sizeof b, "%s %d channel%s, %llu transmissions", t.scan ? "scanning" : "listening to", inBand, inBand == 1 ? "" : "s", (unsigned long long)t.blocksOk);
    return b;
}

} // namespace dect2
