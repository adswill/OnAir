// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GOES HRIT telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include "goes_proto.h"
#include "dect2/common/ring.h"
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kGoesHist = 120;     // histories, one point a report (4 a second), oldest first
constexpr size_t kGoesLogMax = 200;

struct GoesTelemetry {
    uint64_t seq = 0;
    int state = 0;                    // 0 searching, 1 carrier and symbols locked, 2 frames found, 3 frames decoding (Reed-Solomon passes)
    double cfoHz = 0;                 // carrier offset from the dial
    float snrDb = 0;                  // Es/N0 estimated from the symbols
    float levelDb = -200;             // input level, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;            // frames Reed-Solomon passed
    uint64_t blocksBad = 0;           // frames it could not correct
    // details
    double timeSec = 0, inputRate = 0;
    double symbolRate = 0;            // measured by the timing loop
    bool carrierLock = false, frameLock = false;
    bool qpsk = false;                // the demodulator is the QPSK one (GK-2A HRIT)
    bool invertedG2 = true;           // how the convolutional code's second output arrives
    bool rsDualBasis = true;          // Reed-Solomon symbols in the CCSDS dual basis (true) or the conventional one
    float lockMetric = 0;             // 0 .. 1: BPSK constellation quality
    float rsByteErrors = 0;           // fraction of the bytes Reed-Solomon had to correct (last second)
    uint64_t frames = 0, rsCorrected = 0;
    uint64_t vcdus = 0, fillVcdus = 0, vcduGaps = 0, packets = 0, crcErrors = 0, files = 0, fileErrors = 0;
    int scid = -1;
    std::vector<std::pair<int, uint64_t>> vcids;   // virtual channels seen (not fill) and their VCDUs
    std::vector<cf32> constellation;  // recent symbols, unit amplitude
    std::vector<float> snrHist, errHist, rateHist;     // rateHist: frames decoded a second
    std::vector<std::shared_ptr<goes::Product>> products;   // newest first
    std::vector<std::string> log;     // newest first
};

inline std::string goesSummary(const GoesTelemetry& t) {
    char b[200];
    if (t.state >= 2) {
        snprintf(b, sizeof b, "HRIT: %llu frames (%llu bad), %zu products, Es/N0 %.1f dB", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
                 t.products.size(), t.snrDb);
        return b;
    }
    if (t.state == 1) { snprintf(b, sizeof b, "HRIT: carrier locked (%.1f kHz), looking for frames", t.cfoHz / 1000); return b; }
    return "HRIT: searching for the 927 ksym/s BPSK carrier";
}

} // namespace dect2
