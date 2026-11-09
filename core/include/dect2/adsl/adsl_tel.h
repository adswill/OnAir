// ADS-L receiver telemetry, shared by the receiver and the UI.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct AdslTrackPoint { float lat = 0, lon = 0; };

struct AdslAircraft {
    uint32_t id = 0;                 // 24 bit address
    int amt = 0;                     // address table: adsl_proto.h Amt (0 is a random, privacy address)
    int category = 0;
    int flightState = 0;
    int emergency = 1;
    bool hasPos = false; double lat = 0, lon = 0;
    bool hasAlt = false; int altM = 0;           // above the WGS-84 ellipsoid
    bool hasSpeed = false; float speedKmh = 0;
    bool hasTrack = false; float trackDeg = 0;
    bool hasClimb = false; float climbMs = 0;
    int nic = 0, hAcc = 0, vAcc = 0, velAcc = 0;
    bool hasRange = false; float distKm = 0, bearingDeg = 0;
    int band = 0;                    // adsl::Band of the last packet
    bool viaUplink = false;          // last heard relayed through a ground station
    float levelDb = 0;
    uint32_t packets = 0;
    float ageSec = 0;
    bool hasStatus = false;          // the Status payload was heard: its capabilities below
    int maxVersion = 0, mRx = 0, ldrRx = 0, hdrRx = 0, mobile = 0, econRx = 0, econTx = 0, xpdr = 0;
    std::vector<AdslTrackPoint> track;
};

struct AdslTelemetry {
    // ---- the part every mode has: the engine copies it into RxTelemetry; keep these members and what they mean
    uint64_t seq = 0;
    int state = 0;                   // 0 no signal, 1 energy but no good packet lately, 2 receiving packets
    double cfoHz = 0;
    float snrDb = 0;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // packets with good parity / syncs that failed it

    // ---- this mode's own fields
    double timeSec = 0, inputRate = 0;
    float noiseDb = -120, levelDb = -120;
    float packetsPerSec = 0;
    uint64_t perBand[4] = {};        // good packets: 868.2, 868.4, O-band LDR, O-band HDR
    bool bandOn[4] = {};             // inside the capture
    uint64_t traffic = 0, status = 0, uplink = 0, fisB = 0, remoteId = 0, other = 0;
    uint64_t encrypted = 0;          // reserved key index: no key is published, counted only
    uint64_t signedPackets = 0;      // carrying the optional Ed25519 signature (not verified: no public key infrastructure exists)
    uint64_t corrected = 0;          // repaired by the CRC bit search or by Reed-Solomon
    uint64_t syncs = 0;
    bool refValid = false; double refLat = 0, refLon = 0;
    float maxRangeKm = 0;
    uint32_t aircraftCount = 0, withPosition = 0;
    std::vector<AdslAircraft> aircraft;   // most recently heard first
};

inline std::string adslSummary(const AdslTelemetry& t) {
    char b[160];
    if (t.aircraftCount || t.blocksOk) { snprintf(b, sizeof b, "%u aircraft, %.1f packets/s", t.aircraftCount, t.packetsPerSec); return b; }
    if (t.state >= 1) return "signal, no packet yet";
    return "listening on 868.2 / 868.4 / 869.525 MHz";
}

} // namespace dect2
