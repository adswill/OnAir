// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteor-M LRPT telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

constexpr int kMeteorWidth = 1568;        // pixels a line (196 blocks of 8)
constexpr int kMeteorMaxRows = 1500;      // strips of 8 lines kept (12000 lines, longer than any pass)

// 8 lines of one channel. Immutable once published: the receiver replaces a strip (copy on write) when a packet adds to it.
struct MeteorStrip {
    int row = 0;
    uint16_t got = 0;                     // bit k: the packet of blocks 14k .. 14k+13 arrived
    std::vector<uint8_t> px;              // 8 x kMeteorWidth, 0 where nothing arrived
};

// The picture of the pass so far: per channel (APID 64 + i, MSU-MR channel i + 1), a strip per row (nullptr: none yet)
struct MeteorImage {
    uint64_t id = 0;                      // changes when the receiver starts a new picture
    int rows = 0;                         // rows (strips of 8 lines) so far
    std::array<std::vector<std::shared_ptr<const MeteorStrip>>, 6> ch;
    std::array<uint64_t, 6> packets{};    // image packets decoded per channel
};

struct MeteorTelemetry {
    uint64_t seq = 0;
    int state = 0;                        // 0 searching, 1 carrier and symbols, 2 frames (sync word found), 3 frames decoding (RS good)
    double cfoHz = 0;                     // carrier offset found (coarse search plus the Costas loop)
    float snrDb = 0;                      // symbol SNR from the spread of the soft symbols
    float levelDb = -200;                 // input level, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;                // frames (CADUs) with all four RS codewords good
    uint64_t blocksBad = 0;
    // details
    int modulation = 0;                   // the one running: 1 OQPSK 72k (N2-3, N2-4), 2 QPSK 72k (N2), 3 OQPSK 80k interleaved (N2-x)
    int modSetting = 0;                   // 0 automatic, else as above
    double symbolRate = 0, inputRate = 0, timeSec = 0;
    double clockPpm = 0;                  // symbol clock against nominal
    float carrierLock = 0;                // 0 .. 1
    int rotation = -1;                    // phase ambiguity chosen (0 .. 7), -1 none yet
    bool frameLock = false;
    uint64_t asmHits = 0, rsCorrected = 0, vcdus = 0, packets = 0, packetsLost = 0;
    uint64_t apidPackets[8] = {};         // 64 .. 69 the channels, [6] APID 70 (telemetry), [7] others
    int scid = -1, vcid = -1;
    uint32_t vcduCounter = 0;
    uint32_t onboardMs = 0;               // time of day of the last image packet (Moscow time on the satellite)
    int lastQuality = 0;
    std::vector<std::pair<float, float>> constellation;   // recent symbols, normalised
    std::shared_ptr<const MeteorImage> image;
    uint64_t imageSeq = 0;
};

inline const char* meteorModName(int m) {
    return m == 1 ? "OQPSK 72k" : m == 2 ? "QPSK 72k" : m == 3 ? "OQPSK 80k" : "automatic";
}

inline std::string meteorSummary(const MeteorTelemetry& t) {
    char b[160];
    if (t.state >= 3) snprintf(b, sizeof b, "%s, %d lines, %llu frames", meteorModName(t.modulation), t.image ? t.image->rows * 8 : 0, (unsigned long long)t.blocksOk);
    else if (t.state == 2) snprintf(b, sizeof b, "%s, frame sync, RS failing", meteorModName(t.modulation));
    else if (t.state == 1) snprintf(b, sizeof b, "%s, symbols, no frame sync", meteorModName(t.modulation));
    else snprintf(b, sizeof b, "searching (%s)", meteorModName(t.modulation));
    return b;
}

} // namespace dect2
