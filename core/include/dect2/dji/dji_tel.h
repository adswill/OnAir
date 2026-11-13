// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DJI drone telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include "dji_proto.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kDjiTracksMax = 24;       // signal tracks kept (the longest unheard go first)
constexpr size_t kDjiBurstsMax = 300;      // bursts in the log
constexpr size_t kDjiDronesMax = 50;       // drones decoded (by serial number)
constexpr size_t kDjiTrail = 120;          // positions kept per drone
constexpr size_t kDjiRssiHist = 240;       // levels of a track or of the whole capture, one per 0.25 s

// what a signal looks like (DjiTrack::cls); the first four are judged from its shape only
enum DjiClass { kDjiOther = 0, kDjiUnknownOfdm = 1, kDjiLightbridge = 2, kDjiOcusync = 3, kDjiDroneId = 4 };

struct DjiPoint { double lat = 0, lon = 0; float alt = 0; };

// One signal: bursts of about the same frequency and width, with its timing and what it looks like
struct DjiTrack {
    int id = 0;
    int cls = 0;                       // DjiClass
    float conf = 0;                    // 0..1: how well the shape fits the class (never a certain identification, except a DroneID packet with a good CRC)
    double offsetHz = 0;               // the centre of the signal from the radio's centre
    double freqMhz = 0;                // the RF centre (0: unknown, a file)
    double bwMhz = 0;                  // occupied width
    bool clipped = false;              // it fills the capture: it may be wider
    int bursts = 0;                    // bursts seen
    double periodMs = 0;               // typical time between bursts (0: not enough yet)
    float periodQ = 0;                 // 0..1: how regular the bursts are
    double durUs = 0;                  // typical burst length
    float duty = 0;                    // fraction of the time on
    float ofdm = 0;                    // cyclic-prefix correlation 0..1: how OFDM-like (15 / 30 kHz spacing)
    float levelDb = -200;              // dBFS of the last burst
    float snrDb = 0;
    double firstSec = 0, lastSec = 0;
    int64_t lastWall = 0;
    bool zc = false;                   // a burst with the two DroneID Zadoff-Chu symbols was seen
    int decoded = 0, failed = 0;       // DroneID packets decoded / ZC found but the CRC failed
    std::string band, note;
    std::vector<float> rssi;           // levels, one per 0.25 s with a burst, oldest first
    std::vector<float> rssiT;          // their times (signal time)
};

struct DjiBurst {
    double timeSec = 0;
    int64_t wallTime = 0;
    int track = 0;
    double offsetHz = 0;
    float bwMhz = 0, durUs = 0, snrDb = 0, levelDb = 0, ofdm = 0;
    int zc = 0;                        // 0 not tried, 1 tried and no ZC, 2 ZC found, CRC failed, 3 DroneID decoded
};

// One drone, from its DroneID packets (by serial number)
struct DjiDrone {
    dji::DroneId last;                 // the newest packet
    std::string serial;
    std::vector<DjiPoint> track;       // the drone's positions this session
    uint64_t packets = 0;
    double firstSec = 0, lastSec = 0;
    int64_t firstWall = 0, lastWall = 0;
    float snrDb = 0;
    double freqMhz = 0;
    int trackId = 0;
};

struct DjiTelemetry {
    uint64_t seq = 0;
    int state = 0;                     // 0 nothing, 1 wideband bursts, 2 a DJI-like signal, 3 DroneID packets decoded lately
    double cfoHz = 0;                  // the last DroneID burst's carrier offset
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0;             // DroneID packets decoded
    uint64_t blocksBad = 0;            // DroneID bursts (ZC found) whose CRC failed
    // details
    double timeSec = 0, inputRate = 0;
    double centerMhz = 0;              // 0: unknown (a file)
    float noiseDb = -200;              // the noise floor, dBFS per sample
    uint64_t burstsTotal = 0, burstsOfdm = 0;
    double lastDroneIdSec = -1e9;
    std::vector<DjiTrack> tracks;
    std::vector<DjiBurst> bursts;      // newest first
    std::vector<DjiDrone> drones;
    std::vector<float> levelHist;      // the strongest burst level of each 0.25 s (or the noise floor), oldest first
};

inline std::string djiClassName(int c) {
    switch (c) {
    case kDjiDroneId: return "DJI DroneID";
    case kDjiOcusync: return "DJI OcuSync-like";
    case kDjiLightbridge: return "Lightbridge-like";
    case kDjiUnknownOfdm: return "unknown wideband OFDM";
    default: return "other wideband (not OFDM-like)";
    }
}

inline std::string djiSummary(const DjiTelemetry& t) {
    char b[200];
    if (t.state >= 3 && !t.drones.empty()) {
        const DjiDrone* best = &t.drones.front();
        for (const auto& d : t.drones) if (d.lastSec > best->lastSec) best = &d;
        snprintf(b, sizeof b, "DroneID: %s (%llu packets)", best->serial.empty() ? "no serial" : best->serial.c_str(), (unsigned long long)t.blocksOk);
        return b;
    }
    const DjiTrack* top = nullptr;
    for (const auto& k : t.tracks) if (k.cls >= kDjiUnknownOfdm && (!top || k.conf > top->conf)) top = &k;
    if (top) { snprintf(b, sizeof b, "%s, %.0f MHz wide, %.0f%%", djiClassName(top->cls).c_str(), top->bwMhz, top->conf * 100); return b; }
    if (t.state >= 1) { snprintf(b, sizeof b, "%llu wideband bursts, nothing DJI-like yet", (unsigned long long)t.burstsTotal); return b; }
    return "listening";
}

} // namespace dect2
