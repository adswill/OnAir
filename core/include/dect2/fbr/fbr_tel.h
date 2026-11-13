// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Fleet Broadcast (UHF SATCOM, 250.450 / 250.550 / 250.650 MHz) telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr int kFbrChannels = 16;
constexpr size_t kFbrLogMax = 40;
constexpr size_t kFbrFramesShown = 24;
constexpr size_t kFbrBitsShown = 256;

// What one of the 16 time-division channels looks like. Only the bit pattern is classified: nothing is decoded and nothing is decrypted.
struct FbrChannel {
    char cls = '-';        // 'S' repeating pattern (the sync channel), 'r' reversals 1010..., 'i' idle (constant 0 or 1), 'e' random-looking (not decoded), '-' not enough data
    float activity = 0;    // share of frame-to-frame bit changes, 0 .. 1
    int period = 0;        // 'S': the repeat length in frames
    float ones = 0;        // share of 1 bits
    std::string hex;       // the last 64 bits of the channel as 16 hex digits, oldest first
};

struct FbrTelemetry {
    uint64_t seq = 0;
    int state = 0;                  // 0 nothing, 1 a BPSK carrier found, 2 symbols locked (bits flow), 3 frame aligned (channels known)
    double cfoHz = 0;               // carrier offset from the radio's centre as tracked
    float snrDb = 0;                // symbol-to-noise ratio in 1.2 kHz, estimated from the differential decisions
    float levelDb = -200;           // input level, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;          // frames passed since the frame alignment
    uint64_t blocksBad = 0;         // times the alignment was lost
    // the carrier
    double timeSec = 0, inputRate = 0;
    double centerMhz = 0;           // where the radio is tuned (0: not known)
    double offsetHz = 0;            // carrier from the radio's tuned frequency (the Doppler shift shows here)
    double freqMhz = 0;             // absolute (0 when the radio's frequency is not known)
    std::string carrier;            // the listed carrier within 5 kHz ("" none)
    float toneDb = 0;               // peak of the squared signal over its noise floor at acquisition, dB
    // the demodulator
    double baud = 0;                // assumed symbol rate (published: 1200 Bd)
    double clockPpm = 0;            // symbol clock offset measured against it
    float eye = 1;                  // quadrature share of the differential decisions: 0 clean, towards 1 noisy
    uint64_t symbols = 0;
    bool inverted = false;          // the bit polarity switch is on
    std::string bits;               // the last bits as received, '0' / '1', oldest first
    // the frames: 16 bits per frame, one bit per channel (16 x 75 Bd = 1200 Bd)
    bool framed = false;
    bool ambiguous = false;         // several columns repeat: the frame start cannot be told
    int syncPeriod = 0;             // the repeat length of the sync channel found, in frames (0: none)
    int syncQuality = 0;            // agreement of the sync channel with its own repeat, percent
    uint64_t frames = 0;
    std::string status;             // "[Sr-i...]": one letter per channel (S sync, r reversals, i idle, e random-looking, - unknown)
    FbrChannel ch[kFbrChannels];
    std::vector<std::string> frameLog;   // the last frames as 4 hex digits, channel 0 is the top bit, newest last
    std::vector<std::string> log;        // events, newest last
};

inline std::string fbrSummary(const FbrTelemetry& t) {
    char b[220];
    if (t.state == 0) {
        if (t.centerMhz > 0) { snprintf(b, sizeof b, "listening on %.3f MHz for a Fleet Broadcast carrier", t.centerMhz); return b; }
        return "listening for a Fleet Broadcast carrier";
    }
    if (t.state == 1) { snprintf(b, sizeof b, "BPSK carrier at %+.0f Hz, %.0f dB over the floor, looking for the 1200 Bd clock", t.offsetHz, t.toneDb); return b; }
    if (t.state == 2) {
        snprintf(b, sizeof b, "1200 Bd bits, %s", t.ambiguous ? "several repeating columns: frame start not known" : "looking for the sync channel");
        return b;
    }
    snprintf(b, sizeof b, "framed %s, sync repeat %d frames, %llu frames", t.status.c_str(), t.syncPeriod, (unsigned long long)t.frames);
    return b;
}

} // namespace dect2
