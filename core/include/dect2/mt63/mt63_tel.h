// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MT63 receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kMt63TextMax = 4000;
constexpr size_t kMt63LinesMax = 80;
constexpr int kMt63Carriers = 64;

struct Mt63Line {
    double timeSec = 0;          // signal seconds of its first character
    std::string text;
};

struct Mt63Telemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 searching, 1 a signal is being synchronised, 2 decoding text
    double cfoHz = 0;            // how far the AFC moved the carrier comb since it locked
    float snrDb = 0;             // per carrier and symbol, from the phase decisions
    float levelDb = -200;        // input power, dBFS
    bool dataValid = false;      // locked and the Walsh codewords are clean
    uint64_t blocksOk = 0;       // characters decoded
    uint64_t blocksBad = 0;      // symbols whose Walsh codeword was unclear (not shown)
    // details
    int bandwidth = 0;           // Hz: 500, 1000, 2000 once found, else 0
    int interleave = 0;          // 0 unknown, 1 short (32 symbols), 2 long (64)
    bool autoBandwidth = true, autoInterleave = true;
    double centreHz = 0;         // centre of the 64 carriers, Hz above the dial
    double loHz = 0, hiHz = 0;   // the audio window searched (dial + loHz .. hiHz)
    float quality = 0;           // Walsh peak over the rest, smoothed (about 3 for noise, 8 and more for a clean signal)
    float qualityShort = 0, qualityLong = 0;
    float floorDb = -200;        // noise floor of the window spectrum, dB per bin
    double timeSec = 0;
    double inputRate = 0;
    int syncs = 0;               // lock attempts that were accepted
    uint64_t chars = 0;
    std::string text;            // the newest kMt63TextMax characters
    std::vector<Mt63Line> lines; // the chat log, oldest first
    std::vector<std::string> calls;
    std::vector<float> carrierDb;   // level of each of the 64 carriers, dB relative to the strongest
    std::vector<float> spectrum;    // dB per bin, loHz .. hiHz, for the waterfall
    uint64_t specSeq = 0;
};

inline const char* mt63InterleaveName(int i) { return i == 1 ? "short" : i == 2 ? "long" : "?"; }

inline std::string mt63Summary(const Mt63Telemetry& t) {
    char b[200];
    if (t.state == 0 || t.bandwidth == 0) return "searching for MT63 (500 / 1000 / 2000 Hz)";
    if (t.state == 1) { snprintf(b, sizeof b, "MT63-%d: synchronising, centre %.0f Hz", t.bandwidth, t.centreHz); return b; }
    snprintf(b, sizeof b, "MT63-%d %s: decoding, centre %.0f Hz, %.0f dB, %llu chars", t.bandwidth, mt63InterleaveName(t.interleave), t.centreHz, t.snrDb, (unsigned long long)t.chars);
    return b;
}

} // namespace dect2
