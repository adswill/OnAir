// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DECT telemetry, shared by the receiver and the UI. The first members are copied into RxTelemetry: keep them and their meaning.
#pragma once
#include "dect_proto.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kDectBaseMax = 60, kDectLogMax = 300;

struct DectBase {                        // a base station heard on this carrier
    uint64_t rfpi = 0;                   // 40 bits
    int arc = 0, emc = -1, fpn = -1;
    uint32_t slotsMask = 0;              // bit n: a burst in slot n counted from the first one heard (active calls fill more slots)
    float rssi = -200;
    uint64_t frames = 0;                 // beacons with a good CRC
    int ntSeen = 0, qtSeen = 0;
    uint64_t qtLast = 0;                 // the last system-information tail
    double firstSec = 0, lastSec = 0;
    int64_t firstWall = 0, lastWall = 0;
};

struct DectLog {
    int64_t wall = 0;
    double sec = 0;
    bool fromBase = true;
    bool crcOk = false;
    int ta = 0;
    uint64_t tail = 0;
    float rssi = -200;
};

struct DectTelemetry {
    uint64_t seq = 0;
    int state = 0;                       // 0 nothing heard, 1 bursts in the last seconds
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0;               // A-fields with a good CRC
    uint64_t blocksBad = 0;              // sync words found whose A-field CRC failed
    double timeSec = 0, inputRate = 0;
    double channelMhz = 0;               // the centre the radio is tuned to (0: a file)
    int carrier = -1;                    // carrier number of the channel within its band (-1: not one)
    int band = -1;                       // regional band of the channel (dect::band(), -1: none)
    uint64_t baseBursts = 0, handsetBursts = 0;   // sync words heard from base stations and from handsets
    uint64_t crcPlain = 0, crcInverted = 0;       // which CRC form matched
    std::vector<DectBase> bases;         // most recently heard first
    std::vector<DectLog> log;            // newest first
};

inline std::string dectSummary(const DectTelemetry& t) {
    char b[160];
    if (t.blocksOk == 0 && t.baseBursts + t.handsetBursts == 0) return "listening for DECT on the carrier";
    snprintf(b, sizeof b, "%zu base stations, %llu base / %llu handset bursts", t.bases.size(), (unsigned long long)t.baseBursts, (unsigned long long)t.handsetBursts);
    return b;
}

}  // namespace dect2
