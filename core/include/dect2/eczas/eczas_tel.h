// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// e-Czas (Poland, 225 kHz long wave) receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include "eczas_proto.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kEczasHist = 120;      // per-second level / SNR history, oldest first
constexpr size_t kEczasOffsetHist = 90; // clock offsets, one per decoded time packet
constexpr size_t kEczasLogMax = 100;    // frames kept in the log
constexpr size_t kEczasConstel = 240;   // points of the phase plot
constexpr size_t kEczasTrace = 1000;    // samples (500 Hz = 2 s) of the demodulated signal

struct EczasFrameLog {
    int64_t wallTime = 0;               // the computer's clock when the frame was decoded (UTC seconds)
    double streamSec = 0;               // signal time of the frame's first bit
    int kind = 0;                       // 0 time packet, 1 CRC failed, 2 another packet of the minute frame, 3 a time packet that disagrees with the time already received
    bool ok = false;
    std::string text;                   // the time, or why the frame was rejected
    std::string hex;                    // the 12 bytes on air
};

struct EczasTelemetry {
    uint64_t seq = 0;
    int state = 0;                      // 0 no carrier, 1 carrier found (searching for frames), 2 time decoded (last packet within 3 minutes)
    double cfoHz = 0;                   // carrier offset from the dial (what the frequency tracking applies)
    float snrDb = 0;                    // carrier to noise in 100 Hz
    float levelDb = -200;               // carrier level, dBFS
    bool dataValid = false;             // a time packet was decoded within the last 3 minutes
    uint64_t blocksOk = 0;              // time packets with a good CRC
    uint64_t blocksBad = 0;             // time packets with a bad CRC
    // details
    double timeSec = 0, inputRate = 0;
    bool audioInput = false;            // the file holds demodulated audio (a BFO recording), not IQ
    uint64_t syncs = 0, frames = 0, otherType = 0, falseSync = 0;
    bool timeValid = false;             // a time packet has been decoded (it stays shown, see ageSec)
    bool confirmed = false;             // two packets in a row agree with each other and with the elapsed time
    eczas::TimePacket pkt;              // the last one decoded
    double ageSec = 0;                  // signal time since that packet
    double utcNow = 0;                  // the received time (UTC epoch seconds), advanced by the elapsed signal time
    bool clockValid = false;            // the receiver has a clock to compare (a radio or the test signal, not a file)
    double clockOffsetMs = 0;           // computer clock minus the received time at the last packet, after the path delay; positive: the computer is ahead
    double clockOffsetAvgMs = 0;        // averaged over the last packets (up to 10)
    bool locationSet = false;           // the receiver position is the user's (else a placeholder)
    double distanceKm = 0, propDelayMs = 0;
    float syncQuality = 0;              // of the last frame
    std::vector<float> levelHist, snrHist;       // per second
    std::vector<float> offsetHist;               // ms, per time packet, oldest first
    std::vector<float> constelI, constelQ;       // phase plot: the carrier rotated to the real axis, unit mean radius
    std::vector<int16_t> trace;                  // the signal the bit slicer sees (500 Hz), newest last
    std::vector<EczasFrameLog> log;              // newest first
};

inline std::string eczasSummary(const EczasTelemetry& t) {
    char b[200];
    if (t.timeValid) {
        const int64_t u = (int64_t)t.utcNow;
        int y, mo, d, h, mi, s;
        eczas::civilFromUnix(u, y, mo, d, h, mi, s);
        snprintf(b, sizeof b, "e-Czas %04d-%02d-%02d %02d:%02d:%02d UTC (+%d h)", y, mo, d, h, mi, s, t.pkt.tzHours);
        return b;
    }
    if (t.state >= 1) { snprintf(b, sizeof b, "e-Czas: carrier found, %.0f dB, waiting for a time packet", t.snrDb); return b; }
    return "e-Czas: no carrier";
}

} // namespace dect2
