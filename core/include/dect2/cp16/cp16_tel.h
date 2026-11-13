// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CP16 receiver telemetry, shared by the receiver and the interface (cp16_rx.h).
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kCp16PictureRows = 1360;   // dot rows kept for the picture (80 characters)
constexpr size_t kCp16CharsMax = 400;

struct Cp16Char {
    uint64_t serial = 0;
    double timeSec = 0;           // start of its first row, seconds of signal
    std::string text;             // the character (UTF-8), "?" when nothing matched
    uint32_t cp = 0;
    float score = 0;              // correlation with the font's glyph, 0 .. 1
    float margin = 0;             // over the runner-up
    bool sure = false;            // score and margin high enough to be taken as read
};

struct Cp16Telemetry {
    uint64_t seq = 0;
    int state = 0;                // 0 listening, 1 carriers found, 2 rows in step (reading)
    double cfoHz = 0;             // the lowest carrier against the expected position (Advanced: base frequency), Hz
    float snrDb = 0;              // inked dots over blank dots, dB
    bool dataValid = false;
    uint64_t blocksOk = 0;        // characters read with confidence
    uint64_t blocksBad = 0;       // cells with dots that matched no character well
    double inputRate = 0;
    bool rateOk = true;
    float levelDb = -120;
    double nowSec = 0;
    // what the receiver measured (automatic) or was told (Advanced)
    double baseHz = 0;            // audio frequency of the column-0 carrier (upper sideband); with mirror the highest carrier
    double spacingHz = 0;
    double rowSec = 0;
    double charSec = 0;           // 17 rows
    bool mirrored = false;        // lower sideband (or the audio of one): columns run downwards in frequency
    float combScore = 0;          // how clearly 16 equally spaced carriers stand out, dB
    float timingScore = 0;        // coherence of the row edges, 0 .. 1
    std::string fontName;
    // the picture: one entry per dot row, oldest first, 16 brightness values 0 .. 255 (column 0 first)
    std::vector<std::array<uint8_t, 16>> rows;
    uint64_t firstRow = 0;        // serial of rows[0]
    uint64_t rowCount = 0;        // rows made so far
    std::string text;             // characters read (UTF-8), spaces between gaps
    std::vector<Cp16Char> chars;  // newest last
};

inline std::string cp16Summary(const Cp16Telemetry& t) {
    char b[240];
    if (!t.rateOk) {
        snprintf(b, sizeof b, "CP16: sample rate %.0f kHz is too low, use at least 250 kHz", t.inputRate / 1e3);
        return b;
    }
    if (t.state == 0) return "CP16: listening for 16 picture carriers";
    snprintf(b, sizeof b, "CP16: %.0f Hz + 15 x %.1f Hz, %.2f s a character%s, %llu read, %llu unclear", t.baseHz, t.spacingHz, t.charSec,
             t.mirrored ? ", mirrored" : "", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    return b;
}

} // namespace dect2
