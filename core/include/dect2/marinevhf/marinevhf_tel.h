// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Marine VHF telemetry. RxTelemetry::marinevhf, engine standard 157, active standard 156 (family 155).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// What one carrier receiver (one channel) reports to the bank
struct MarineVhfCarrierTel {
    uint64_t seq = 0;
    int state = 0;                    // 0 listening, 2 squelch open
    double cfoHz = 0;
    float snrDb = 0;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;
    bool open = false;
    double levelDb = -200;            // channel power, dB relative to full scale
    double quietDb = -30;             // the squelch measure: speech band over the noise band of the discriminator output
};

// One row of the activity list: a channel of the region's plan
struct MarineVhfChannelRow {
    int key = -1;                     // index in marinevhf::channels(region)
    int number = 0;
    char suffix = 0;
    double mhz = 0;                   // the frequency carrying the signal now (or the ship frequency)
    bool active = false;              // a carrier is on the channel now
    bool open = false;                // and the squelch is open (voice)
    bool playing = false;
    bool inScan = false;
    double levelDb = -200;
    int calls = 0;
    double totalSec = 0;
    double lastHeardAgoSec = -1;      // -1 never
    int64_t lastHeardWall = 0;
    double peakDb = -200;
};

// One transmission
struct MarineVhfCall {
    uint64_t serial = 0;
    int key = -1;
    int number = 0;
    char suffix = 0;
    double mhz = 0;
    int64_t wallTime = 0;
    double startSec = 0, durationSec = 0;
    double peakDb = -200, meanDb = -200;
    bool open = false;
    std::string file;                 // the recording ("" when none)
};

struct MarineVhfTelemetry {
    uint64_t seq = 0;
    int state = 0;                    // 0 no input, 1 listening, 2 a voice channel is open
    double cfoHz = 0;
    float snrDb = 0;
    bool dataValid = false;           // at least one transmission heard
    uint64_t blocksOk = 0, blocksBad = 0;
    double timeSec = 0, inputRate = 0, centerHz = 0;
    int region = 0;                   // marinevhf::Region
    bool spanCovers = false;          // the capture is wider than one channel (whole-span monitoring)
    int carriers = 0;                 // carriers tracked now
    int playingKey = -1;              // the channel whose audio plays
    int playingNumber = 0;
    char playingSuffix = 0;
    double playingMhz = 0;
    int scanState = 0;                // ChannelScan::State
    bool scanOn = false;
    int recordedFiles = 0;
    uint64_t callsTotal = 0;
    std::vector<MarineVhfChannelRow> rows;   // every channel of the region, in plan order
    std::vector<MarineVhfCall> calls;        // newest first
};

constexpr size_t kMarineVhfCallMax = 600;

inline std::string marineVhfSummary(const MarineVhfTelemetry& t) {
    char b[160];
    if (t.state == 0) return "Marine VHF: waiting for samples";
    int act = 0;
    for (const auto& r : t.rows) if (r.active) act++;
    if (t.playingNumber) snprintf(b, sizeof b, "Marine VHF: listening to ch %d%c, %d active, %llu calls", t.playingNumber, t.playingSuffix ? t.playingSuffix : ' ', act, (unsigned long long)t.callsTotal);
    else snprintf(b, sizeof b, "Marine VHF: %d channels active, %llu calls", act, (unsigned long long)t.callsTotal);
    return b;
}

} // namespace dect2
