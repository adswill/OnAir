// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ANT+ telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include "ant_proto.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kAntDevMax = 100;       // sensors kept (the longest unheard go first)
constexpr size_t kAntLogMax = 400;       // frames in the log
constexpr size_t kAntHrHist = 2400;      // heart rate samples kept per sensor (about 10 minutes at 4 a second)

struct AntSample { double sec; float value; };   // signal seconds, bpm (or W, rpm, km/h for the other profiles)

// One sensor: a device number, type and transmission type
struct AntDevice {
    uint16_t deviceNumber = 0;
    int type = 0;                        // ant::DeviceType (low 7 bits)
    uint8_t transType = 0;
    uint16_t marker = 0;                 // network marker (0xA6C5 ANT+)
    bool pairing = false;                // the pairing request bit was seen
    std::string typeName;                // "Heart rate monitor"
    std::string manufacturer;            // from the manufacturer page, when sent
    int serial = -1, model = -1, hwRev = -1, swRev = -1;
    int batteryPct = -1, batteryStatus = -1;
    double batteryV = -1, operatingTimeSec = -1;
    // the newest reading
    int heartRate = -1, beatCount = -1;
    double rrSec = -1;
    int powerW = -1, cadenceRpm = -1;
    double speedKmh = -1, distanceM = -1, temperatureC = -1000;
    std::string lastPage, lastText;
    std::vector<AntSample> history;      // heart rate (bpm), or power (W), speed (km/h), temperature: the profile's main value
    std::string historyUnit = "bpm";
    uint32_t pagesSeen = 0;              // bit n: page n seen (n < 32)
    float rssi = -200;                   // dBFS of the last frame
    double firstSec = 0, lastSec = 0;    // signal time
    int64_t firstWall = 0, lastWall = 0;
    uint64_t frames = 0, crcErrors = 0;
    double periodSec = 0;                // measured message period (the profile's rate)
    // the counters the speed / cadence profiles derive from (last event time and count)
    int lastCadTime = -1, lastCadRevs = -1, lastSpdTime = -1, lastSpdRevs = -1;
};

struct AntFrameLog {
    int64_t wall = 0;
    double sec = 0;
    uint16_t deviceNumber = 0;
    int type = 0;
    uint8_t transType = 0;
    uint16_t marker = 0;
    bool crcOk = true;
    float rssi = -200;
    uint8_t payload[ant::kPayload] = {};
    std::string text;                    // the decoded page
};

struct AntTelemetry {
    uint64_t seq = 0;
    int state = 0;                       // 0 nothing heard, 1 frames in the last seconds
    double cfoHz = 0;                    // carrier offset of the last good frame
    float snrDb = 0;                     // eye SNR of the last good frames
    float levelDb = -200;                // input level, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;               // frames with a good CRC
    uint64_t blocksBad = 0;              // frames found whose CRC failed
    // details
    double timeSec = 0, inputRate = 0;
    uint64_t preambleHits = 0;
    float deviationHz = 0;
    double channelMhz = 0;               // the channel the radio is tuned to (0: a file)
    std::vector<AntDevice> devices;      // most recently heard first
    std::vector<AntFrameLog> log;        // newest first, at most kAntLogMax
};

inline std::string antSummary(const AntTelemetry& t) {
    char b[160];
    for (const AntDevice& d : t.devices) {
        if (d.type == ant::HeartRate && d.heartRate > 0) { snprintf(b, sizeof b, "%d bpm (sensor %u), %zu sensors, %llu frames", d.heartRate, d.deviceNumber, t.devices.size(), (unsigned long long)t.blocksOk); return b; }
    }
    if (t.blocksOk == 0) return "listening for ANT+ sensors on 2457 MHz";
    snprintf(b, sizeof b, "%zu sensors, %llu frames", t.devices.size(), (unsigned long long)t.blocksOk);
    return b;
}

} // namespace dect2
