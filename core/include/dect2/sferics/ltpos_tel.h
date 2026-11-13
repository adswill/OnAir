// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Lightning position (network) telemetry, shared by the receiver and the UI. Receive only; nothing is transmitted on air.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kLtStrikesMax = 500;    // strikes kept for the map and the list
constexpr size_t kLtStationsMax = 64;    // stations in the server's table

struct LtStationRow {
    uint32_t id = 0;              // station number (from its name)
    std::string name;
    double lat = 0, lon = 0;      // its position, degrees
    int timeSource = 0;           // ltpos::TimeSrc of its latest detections
    float sigmaUs = 0;            // its timing error, 1 sigma, microseconds (after the server's common-view correction)
    float ageS = -1;              // seconds since its last packet
    uint64_t detections = 0;      // detections received
    uint64_t used = 0;            // of them used in a position fix
    bool fine = false;            // timed well enough to take part in a fix
    bool tiedUtc = true;          // its clock is tied to UTC (else only to the common-view reference)
    float corrUs = 0;             // the common-view correction applied, microseconds
};

struct LtStrikeRow {
    int64_t utcNs = 0;            // strike time, nanoseconds since 1970 (UTC when a GPS-timed station took part)
    double lat = 0, lon = 0;      // position, degrees
    float majorKm = 0, minorKm = 0, azDeg = 0;   // 95 % error ellipse: semi-axes, azimuth of the major axis
    int stations = 0;             // fine-timed stations in the fix
    int alsoHeard = 0;            // coarse-timed stations whose detection fits
    float rmsUs = 0;              // rms of the arrival-time residuals, microseconds
    float chi2 = 0;
    int dof = 0;
    bool unchecked = false;       // three stations: nothing could reject a wrong match
    bool ambiguous = false;       // a second position fits as well
    bool utc = true;              // the time is UTC (else offset by the reference's unknown epoch)
};

struct LtPosTelemetry {
    uint64_t seq = 0;
    int state = 0;                // 0 no samples, 1 measuring the noise floor, 2 running, no network, 3 running with the network
    double cfoHz = 0;
    float snrDb = 0;              // the last sferic over the floor, dB
    bool dataValid = false;       // at least one sferic timed
    uint64_t blocksOk = 0;        // strikes located (server) or detections sent (client)
    uint64_t blocksBad = 0;       // packets refused (authentication, replay, malformed)
    int role = 0;                 // 0 this station only, 1 client (sends to a server), 2 server (collects and locates)
    // this station's clock
    int timeSource = 0;           // ltpos::TimeSrc stamping the detections now
    float clockSigmaUs = 0;       // its estimated error, 1 sigma, microseconds
    double ppm = 0;               // the radio's sample clock error against that source
    float ppmSigma = 0;
    bool fine = false;            // good enough for a position fix
    std::vector<float> srcSigmaUs;   // per ltpos::TimeSrc 1..6: the error each source gives now, -1 when it is not available
    uint64_t pulses = 0;          // sferics timed here
    uint64_t sent = 0;            // detections sent to the server
    uint64_t sendErrors = 0;
    float floorDb = -200;         // noise floor, dBFS
    float thresholdDb = 12;
    // common-view reference
    bool refOn = false;
    bool refLocked = false;
    std::string refName;
    float refSnrDb = 0;
    float refSigmaUs = 0;         // one block's symbol timing error, microseconds
    double refFreqErrHz = 0;
    // network
    bool listening = false;       // the server's port is open
    std::string netStatus;        // one line: "sending to ...", "listening on port ...", an error
    uint64_t packetsOk = 0, packetsAuth = 0, packetsReplay = 0, packetsBad = 0;
    std::vector<LtStationRow> stations;
    std::vector<LtStrikeRow> strikes;    // newest first
    uint64_t groups = 0, fixes = 0, rejected = 0, unmatched = 0, coarseOnly = 0;
    double timeSec = 0;
    double inputRate = 0;
    double centerHz = 0;
};

}  // namespace dect2
