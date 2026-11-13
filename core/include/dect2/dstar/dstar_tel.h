// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// D-STAR receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct DstarPosition {
    int64_t wallTime = 0;
    std::string call;            // MY of the transmission (or the D-PRS source)
    double lat = 0, lon = 0, altM = 0, speedKmh = 0;
    int course = 0;
    bool altValid = false, velValid = false;
    bool dprs = false;           // from a D-PRS (APRS) line, else NMEA
    std::string comment;
};

struct DstarMessage {
    int64_t wallTime = 0;
    std::string my, ur;
    std::string text;            // the 20-character slow data message
};

// One transmission (call), for the call log and the "now" panel
struct DstarCall {
    int carrierId = 0; double carrierHz = 0;   // whole-span decoding: the carrier it came from (id 0: single channel), its frequency relative to the span centre
    int64_t wallTime = 0;        // Unix time of its start
    double startSec = 0;         // signal seconds
    double durSec = 0;
    bool active = false;
    bool headerOk = false;       // the radio header passed its CRC
    bool headerFromSlow = false; // the header came from the slow data resend (the radio header was missed or damaged)
    bool headerRepaired = false; // the header's CRC failed; a heard callsign put back made it pass (dstar_repair.h)
    bool lateEntry = false;      // found from the data sync in the middle of the call
    bool endSeen = false;        // the end-of-transmission pattern ended it (else the signal was lost)
    uint8_t flag[3] = {0, 0, 0};
    std::string flags;           // as text
    std::string my, sfx, ur, rpt1, rpt2;   // "" until known
    std::string text;            // slow data message (20 characters)
    int textBlocks = 0;          // which of its 4 blocks arrived (bits)
    bool hasPos = false;
    double lat = 0, lon = 0;
    std::string gps;             // the last GPS line
    uint32_t voiceFrames = 0;
    uint32_t syncOk = 0, syncMiss = 0;
    int squelch = -1;            // digital code squelch, -1 none
    bool voice = false;          // AMBE was decoded to sound
    float berPct = 0;            // channel bit errors: header (corrected by its FEC) and data syncs
};

struct DstarTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 searching, 1 signal seen recently, 2 receiving a call
    double cfoHz = 0;            // carrier offset measured from the bits
    float snrDb = 0;             // bit SNR at the decision points
    float levelDb = -200;        // input power, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;       // voice frames received
    uint64_t blocksBad = 0;      // data syncs missed
    // details
    double timeSec = 0;
    double inputRate = 0;
    float deviationHz = 0;       // as measured on the bit sync (alternating bits; about 0.7 of the 1200 Hz peak deviation)
    bool inverted = false;       // the frequency comes upside down (mirrored spectrum or a receiver that inverts)
    float berPct = 0;
    uint64_t headersOk = 0, headersBad = 0, headersRepaired = 0, lateEntries = 0, slowHeaders = 0, endPatterns = 0, gpsLines = 0, gpsBad = 0;
    uint64_t voiceFrames = 0;    // AMBE frames received
    uint64_t voiceDecoded = 0;   // AMBE frames decoded to sound (mbelib)
    bool ambe = false;           // mbelib found
    std::string ambePath;
    DstarCall now;               // the current (or last) call
    std::vector<DstarCall> log;           // newest first, at most kDstarLogMax
    std::vector<DstarMessage> messages;   // newest first
    std::vector<DstarPosition> positions; // newest first
};

constexpr size_t kDstarLogMax = 300;

inline std::string dstarSummary(const DstarTelemetry& t) {
    char b[200];
    const DstarCall& n = t.now;
    if (n.active) {
        snprintf(b, sizeof b, "%s%s%s -> %s, %.1f s", n.my.empty() ? "?" : n.my.c_str(), n.sfx.empty() ? "" : "/", n.sfx.c_str(), n.ur.empty() ? "?" : n.ur.c_str(), n.durSec);
        return b;
    }
    if (t.headersOk + t.lateEntries == 0) return "searching for D-STAR";
    snprintf(b, sizeof b, "listening, %zu calls heard", t.log.size());
    return b;
}

} // namespace dect2
