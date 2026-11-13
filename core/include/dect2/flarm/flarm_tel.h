// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FLARM receiver telemetry, shared by the receiver and the UI.
// Privacy: only aircraft that broadcast openly are in the table. An aircraft that sets the no-track or stealth flag (or an anonymous address) is
// never stored, shown, saved or forwarded; the receiver only counts them (privacyFlagged).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct FlarmTrackPoint { float lat = 0, lon = 0; };

struct FlarmAircraft {
    uint32_t id = 0;                // 24 bit device ID
    int addrType = 2;               // 0 random, 1 ICAO, 2 FLARM
    int acType = 0;                 // FLARM aircraft type 0 - 15 (flarm_proto.h)
    bool hasPos = false; double lat = 0, lon = 0;
    int altM = 0;                   // metres above the WGS84 ellipsoid
    float speedKmh = 0, trackDeg = 0, climbMs = 0;
    bool airborne = true;
    bool hasRange = false; float distKm = 0, bearingDeg = 0;   // from the receiver position
    int channel = 0;                // 0 868.2 MHz, 1 868.4 MHz
    float levelDb = 0;              // signal level over the noise floor of the last packet
    uint32_t packets = 0;
    float ageSec = 0;
    std::vector<FlarmTrackPoint> track;   // recent positions, oldest first (map)
};

struct FlarmTelemetry {
    // ---- the part every mode has: the engine copies it into RxTelemetry; keep these members and what they mean
    uint64_t seq = 0;
    int state = 0;                  // 0 no signal, 1 energy on a channel but no good packet lately, 2 receiving packets
    double cfoHz = 0;               // 0: not measured
    float snrDb = 0;                // mean level of the last good packets over the noise floor
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // packets with a good CRC (old protocol, position decoded) / syncs that failed the CRC or the parity

    // ---- this mode's own fields
    double timeSec = 0, inputRate = 0;
    float noiseDb = -120, levelDb = -120;
    float packetsPerSec = 0;
    uint64_t perChannel[2] = {};    // good packets per channel
    uint64_t newProtocol = 0;       // packets of the newer protocol (CRC good, not decodable: counted only)
    uint64_t otherType = 0;         // CRC good, another packet type
    uint64_t syncs = 0;             // sync words found
    uint64_t privacyFlagged = 0;    // distinct aircraft that set no-track / stealth, plus anonymous ones: a count, nothing else is kept
    uint64_t privacyPackets = 0;
    uint64_t newProtocolDevices = 0;   // distinct senders of the newer protocol (a count only)
    bool refValid = false; double refLat = 0, refLon = 0;
    bool clockFromWall = true;      // the key time of the packets comes from the computer's clock
    float maxRangeKm = 0;
    uint32_t aircraftCount = 0, withPosition = 0;
    std::vector<FlarmAircraft> aircraft;    // most recently heard first
};

inline std::string flarmSummary(const FlarmTelemetry& t) {
    char b[200];
    if (t.aircraftCount || t.blocksOk) {
        snprintf(b, sizeof b, "%u aircraft, %.1f packets/s%s", t.aircraftCount, t.packetsPerSec,
                 t.privacyFlagged ? (", " + std::to_string((unsigned long long)t.privacyFlagged) + " privacy-flagged").c_str() : "");
        return b;
    }
    if (t.newProtocol) { snprintf(b, sizeof b, "FLARM (new protocol): %llu packets, no positions", (unsigned long long)t.newProtocol); return b; }
    if (t.state >= 1) return "signal, no packet yet";
    return "listening on 868.2 / 868.4 MHz";
}

} // namespace dect2
