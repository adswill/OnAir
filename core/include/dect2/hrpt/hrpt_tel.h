// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HRPT / AHRPT telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

constexpr int kHrptWidth = 2048;          // pixels a scan line
constexpr int kHrptChannels = 5;          // AVHRR channels in a line
constexpr int kHrptMaxLines = 4000;       // lines kept (about 11 minutes at 6 lines a second)

// One AVHRR scan line, 8 bit (the top eight of the ten bits), channel major: px[c * kHrptWidth + x]. Immutable once published.
struct HrptLine {
    int idx = 0;                          // line number in the picture (gaps where packets were lost)
    uint32_t ms = 0;                      // on-board time of day, ms
    std::vector<uint8_t> px;
};

struct HrptImage {
    uint64_t id = 0;                      // changes when the receiver starts a new picture
    int lines = 0;                        // lines so far (highest index + 1)
    std::vector<std::shared_ptr<const HrptLine>> line;   // by index; nullptr where nothing arrived
};

struct HrptTelemetry {
    uint64_t seq = 0;
    int state = 0;                        // 0 searching, 1 carrier and symbols, 2 frame sync, 3 frames good
    double cfoHz = 0;                     // carrier offset (coarse search plus the Costas loop)
    float snrDb = 0;                      // symbol SNR from the spread of the soft symbols
    float levelDb = -200;                 // input level, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;                // frames: CADUs with all RS codewords good (AHRPT) or minor frames with their sync words (NOAA)
    uint64_t blocksBad = 0;
    // details
    int variant = 0;                      // the one running: 1 MetOp AHRPT, 2 FY-3 AHRPT, 3 NOAA HRPT
    int varSetting = 0;                   // 0 automatic, else as above
    double symbolRate = 0, inputRate = 0, timeSec = 0;
    double clockPpm = 0;
    float carrierLock = 0;                // 0 .. 1
    int rotation = -1;                    // AHRPT phase ambiguity chosen (0 .. 7); NOAA: polarity + 2 * Manchester phase; -1 none yet
    bool frameLock = false;
    uint64_t asmHits = 0, rsCorrected = 0, vcdus = 0, packets = 0, packetsLost = 0;
    uint64_t apidPackets[4] = {};         // [0] APID 103 (AVHRR with 3A), [1] APID 104 (AVHRR with 3B), [2] NOAA minor frames, [3] other
    uint64_t vcidFrames[64] = {};         // frames per virtual channel (AHRPT)
    int scid = -1, vcid = -1;
    uint32_t vcduCounter = 0;
    uint64_t lines = 0;                   // AVHRR lines decoded
    uint32_t onboardMs = 0;               // time of day of the last line
    std::vector<std::pair<float, float>> constellation;   // recent symbols, normalised
    std::shared_ptr<const HrptImage> image;
    uint64_t imageSeq = 0;
};

inline const char* hrptVarName(int v) {
    return v == 1 ? "MetOp AHRPT" : v == 2 ? "FY-3 AHRPT" : v == 3 ? "NOAA HRPT" : "automatic";
}

inline std::string hrptSummary(const HrptTelemetry& t) {
    char b[160];
    if (t.state >= 3) snprintf(b, sizeof b, "%s, %d lines, %llu frames", hrptVarName(t.variant), t.image ? t.image->lines : 0, (unsigned long long)t.blocksOk);
    else if (t.state == 2) snprintf(b, sizeof b, "%s, frame sync, frames failing", hrptVarName(t.variant));
    else if (t.state == 1) snprintf(b, sizeof b, "%s, symbols, no frame sync", hrptVarName(t.variant));
    else snprintf(b, sizeof b, "searching (%s)", hrptVarName(t.variant));
    return b;
}

} // namespace dect2
