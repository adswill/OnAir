// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CATS (balloon packet radio, #643) telemetry, shared by the receiver and the UI. The first members are copied into RxTelemetry (history plot, top bar).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kCatsPacketsMax = 100;
constexpr size_t kCatsEventsMax = 60;
constexpr size_t kCatsHist = 120;

struct CatsPacketRow {
    int64_t utc = 0;           // when it was received
    double timeSec = 0;
    std::string callsign;
    int ssid = 0, icon = 0;
    bool hasTime = false;
    int64_t unixTime = 0;      // the packet's own timestamp
    bool hasGps = false;
    double lat = 0, lon = 0, altM = 0, speedMps = 0, headingDeg = 0;
    std::string comment;
    int whiskers = 0;
};

struct CatsEvent {
    int64_t utc = 0;
    double timeSec = 0;
    std::string text;
};

struct CatsTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 nothing heard, 1 packets decoded
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0;       // packets decoded (CRC good)
    uint64_t blocksBad = 0;
    double timeSec = 0, inputRate = 0;
    std::vector<CatsPacketRow> packets;   // newest first
    std::vector<CatsEvent> events;        // newest first
    std::vector<float> levelHist;
};

inline std::string catsSummary(const CatsTelemetry& t) {
    char b[200];
    if (t.state == 0) return "CATS: no packet yet";
    const CatsPacketRow* g = nullptr;
    for (const auto& p : t.packets) if (p.hasGps) { g = &p; break; }
    if (g) snprintf(b, sizeof b, "CATS: %llu packets, %s-%d at %.4f, %.4f, %.0f m", (unsigned long long)t.blocksOk, g->callsign.c_str(), g->ssid, g->lat, g->lon, g->altM);
    else snprintf(b, sizeof b, "CATS: %llu packets", (unsigned long long)t.blocksOk);
    return b;
}

} // namespace dect2
