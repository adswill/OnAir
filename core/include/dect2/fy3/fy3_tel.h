// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FY-3 (Fengyun-3 polar orbit) AHRPT and MPT telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

constexpr int kFy3MaxRows = 4000;         // rows kept in a picture (a 10 minute pass is about 3600 VIRR or 4000 MERSI rows)
constexpr int kFy3MaxChannels = 32;

// One picture row of an instrument, 8 bit (the top eight bits of the 10 or 12 bit value), channel major: px[c * width + x]. Immutable once published.
struct Fy3Row {
    int idx = 0;                          // row number in the picture (gaps where frames were lost)
    uint32_t ms = 0;                      // on-board time of day, ms (0 when unknown)
    std::vector<uint8_t> px;
};

struct Fy3Image {
    uint64_t id = 0;                      // changes when a new picture starts
    int inst = 0;                         // Fy3Inst (fy3_proto.h)
    int width = 0, channels = 0;
    int rows = 0;                         // rows so far (highest index + 1)
    std::vector<std::shared_ptr<const Fy3Row>> row;   // by index; nullptr where nothing arrived
};

// An instrument that was heard (frames of its virtual channel)
struct Fy3InstStat {
    int inst = 0;
    uint64_t frames = 0;                  // CADUs of its virtual channel
    uint64_t units = 0;                   // lines / scans decoded (images) or packets (the others)
    bool image = false;
};

struct Fy3Telemetry {
    uint64_t seq = 0;
    int state = 0;                        // 0 searching, 1 carrier and symbols, 2 frame sync, 3 frames good
    double cfoHz = 0;                     // carrier offset (coarse search plus the Costas loop)
    float snrDb = 0;                      // symbol SNR (Es/N0) from the spread of the soft symbols
    float levelDb = -200;                 // input level, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;                // CADUs with all four Reed-Solomon codewords good
    uint64_t blocksBad = 0;
    // details
    int product = 0;                      // 0 AHRPT, 1 MPT
    int satSetting = 0;                   // 0 automatic, else the preset index (fy3Sats)
    int sat = -1;                         // preset index of the satellite heard (from the spacecraft id), -1 none yet
    double symbolRate = 0, inputRate = 0, timeSec = 0;
    double rateMeasured = 0;              // the symbol rate the automatic search measured (0: not measured, the preset's was used)
    double clockPpm = 0;
    float carrierLock = 0;                // 0 .. 1
    bool frameLock = false;
    bool railLock = false;                // both convolutional decoders have their phase
    int railPhase[2] = {-1, -1};          // code-pair transform chosen per rail (0 .. 7) and the pair alignment in bit 4 (value 8 .. 15 = alignment 1); -1 none
    int diffMode = -1;                    // I/Q swap and Q polarity hypothesis (0 .. 7), -1 none yet
    float railMetric[2] = {};             // quality of each rail's code lock, 0 .. 1 (1 clean)
    float viterbiBer = 0;                 // re-encoded mismatch rate of the Viterbi decoders (an estimate of the channel bit error rate)
    uint64_t asmHits = 0, rsCorrected = 0, vcdus = 0;
    uint64_t vcidFrames[64] = {};
    int scid = -1, vcid = -1;
    uint32_t vcduCounter = 0;
    uint64_t lines = 0;                   // image rows decoded, all instruments
    uint32_t onboardMs = 0;
    std::vector<Fy3InstStat> inst;
    std::vector<std::pair<float, float>> constellation;   // recent symbols, normalised
    std::vector<std::shared_ptr<const Fy3Image>> images; // one per instrument with a picture
    uint64_t imageSeq = 0;
    std::string hint;                     // why nothing decodes yet (one sentence), empty when it does
};

inline std::string fy3Summary(const Fy3Telemetry& t) {
    static const char* const kSat[8] = {"FY-3A", "FY-3B", "FY-3C", "FY-3D", "FY-3E", "FY-3F", "FY-3G", "FY-3H"};
    const char* name = t.sat >= 0 && t.sat < 8 ? kSat[t.sat] : "FY-3";
    char b[160];
    if (t.state >= 3) snprintf(b, sizeof b, "%s %s, %llu rows, %llu frames", name, t.product ? "MPT" : "AHRPT", (unsigned long long)t.lines, (unsigned long long)t.blocksOk);
    else if (t.state == 2) snprintf(b, sizeof b, "%s, frame sync, frames failing", t.product ? "FY-3 MPT" : "FY-3 AHRPT");
    else if (t.state == 1) snprintf(b, sizeof b, "%s, carrier, %s", t.product ? "FY-3 MPT" : "FY-3 AHRPT", t.railLock ? "code locked, no frame sync" : "no code lock");
    else snprintf(b, sizeof b, "searching (%s)", t.product ? "FY-3 MPT" : "FY-3 AHRPT");
    return b;
}

} // namespace dect2
