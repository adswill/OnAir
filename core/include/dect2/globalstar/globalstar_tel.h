// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Globalstar activity monitor telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
// Receive only: the CDMA signal (the L-band terminal uplink) is encrypted and its content is neither decoded nor decodable here; this is power, occupancy and Doppler.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr int kGsChannels = 13;              // the 1.23 MHz CDMA channels of the 1610 - 1626.5 MHz terminal uplink band
constexpr double kGsChanHz = 1.23e6;         // channel spacing
constexpr double kGsFirstHz = 1611.25e6;     // centre of channel 1
constexpr double kGsDefaultDialHz = 1618.25e6;   // the band centre: assumed for a file without a tuned frequency
constexpr size_t kGsTrackMax = 2400;         // Doppler points kept
constexpr size_t kGsEventsMax = 200;

inline double globalstarChannelHz(int idx) { return kGsFirstHz + kGsChanHz * idx; }   // idx 0 .. 12

struct GsChannel {
    int index = 0;                  // 0 .. 12 (channel number - 1)
    double centreMhz = 0;
    bool inView = false;            // inside the sampled band
    bool active = false;
    float powerDb = -120;           // power in the channel, dBFS
    float noiseDb = -120;           // its noise floor
    float snrDb = 0;                // power above the floor, in the channel bandwidth
    float occupancy = 0;            // share of the last minute the channel was active, 0 .. 1
    double dopplerHz = 0;           // spectral centroid against the channel centre while active
    double lastActiveSec = -1e9;
    uint32_t events = 0;
};

struct GsDopplerPoint { double timeSec = 0; int chan = 0; float hz = 0; float snrDb = 0; };

struct GsEvent {                    // one stretch of activity on one channel
    double startSec = 0, durationSec = 0;
    int64_t wallTime = 0;           // the computer's clock at the start (0: a file)
    int chan = 0;
    float peakSnrDb = 0;
    float dopplerStartHz = 0, dopplerEndHz = 0;
};

struct GlobalstarTelemetry {
    uint64_t seq = 0;
    int state = 0;                  // 0 nothing, 1 activity seen within the last 30 s, 2 a channel active now
    double cfoHz = 0;               // mean Doppler of the active channels
    float snrDb = 0;                // strongest active channel
    bool dataValid = false;         // a channel was active in the last 5 s
    uint64_t blocksOk = 0;          // activity events completed
    uint64_t blocksBad = 0;         // always 0 (nothing is decoded)

    double timeSec = 0, inputRate = 0;
    double dialMhz = 0;             // 0: unknown (a file)
    int activeNow = 0;
    std::vector<GsChannel> chan;    // the 13 channels (inView says which the radio covers)
    std::vector<GsDopplerPoint> track;   // oldest first
    std::vector<GsEvent> events;    // newest first
};

inline std::string globalstarSummary(const GlobalstarTelemetry& t) {
    char b[160];
    if (t.activeNow > 0) { snprintf(b, sizeof b, "%d channel%s active, Doppler %+.1f kHz, SNR %.0f dB", t.activeNow, t.activeNow == 1 ? "" : "s", t.cfoHz / 1e3, t.snrDb); return b; }
    if (t.state >= 1) return "activity seen lately, idle now";
    return "listening: no Globalstar terminal uplink";
}

} // namespace dect2
