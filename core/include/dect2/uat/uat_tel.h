// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// UAT (978 MHz) telemetry for the receiver and UI.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct UatAircraft {
    uint32_t icao = 0;
    std::string callsign;
    bool hasSquawk = false;
    int squawk = 0;
    bool hasAlt = false;
    int altFt = 0;
    bool hasGeoAlt = false;
    int geoAltFt = 0;
    bool hasPos = false;
    double lat = 0, lon = 0;
    bool hasSpeed = false;
    int speedKt = 0;
    bool hasHeading = false;
    float headingDeg = 0;
    bool hasVrate = false;
    int vrateFpm = 0;
    bool hasRange = false;
    float distNm = 0, bearingDeg = 0;
    bool ground = false;
    bool tisb = false;
    int nic = -1;
    uint32_t messages = 0;
    float ageSec = 0;
    float levelDbfs = -120;
    std::vector<std::pair<float, float>> track;   // lat, lon
};

struct UatFisbProduct {
    uint16_t productId = 0;
    std::string title;
    std::string text;
    double timeSec = 0;
};

struct UatNexradCell { float lat, lon, dLat, dLon; uint8_t value; };   // one bin of a NEXRAD block

struct UatFrameInfo {
    std::string hex;
    std::string kind;            // "ADS-B", "TIS-B", "FIS-B", "UAT"
    uint32_t address = 0;
    double timeSec = 0;
    float levelDbfs = 0;
    std::string summary;
};

struct UatTelemetry {
    uint64_t seq = 0;
    int state = 0;
    double cfoHz = 0;
    float snrDb = 0;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;

    double timeSec = 0;
    float msgsPerSec = 0;
    float noiseDbfs = -120;
    uint64_t frameCount = 0;
    uint32_t aircraftCount = 0;
    uint32_t withPosition = 0;
    uint32_t fisbCount = 0;
    uint32_t tisbCount = 0;
    uint32_t otherCount = 0;
    bool refValid = false;
    double refLat = 0, refLon = 0;
    std::vector<UatAircraft> aircraft;
    std::vector<UatFisbProduct> fisb;
    std::vector<UatNexradCell> nexrad;   // bins with an echo (value >= 2) of the latest radar blocks, at most kUatNexradMax
    std::vector<UatFrameInfo> framesLog;
};

constexpr size_t kUatNexradMax = 20000;

inline std::string uatSummary(const UatTelemetry& t) {
    char b[220];
    snprintf(b, sizeof b, "UAT: state %d  %.0f msg/s  %u aircraft (%u pos)  FIS-B %u  TIS-B %u  SNR %.1f dB  ok %llu  bad %llu",
             t.state, t.msgsPerSec, t.aircraftCount, t.withPosition, t.fisbCount, t.tisbCount, t.snrDb,
             (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    return b;
}

} // namespace dect2
