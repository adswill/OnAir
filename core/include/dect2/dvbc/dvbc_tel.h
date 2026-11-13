// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// What the DVB-C receiver reports (RxTelemetry::dvbc, standard 41).
#pragma once
#include "docsis_down.h"
#include "dect2/common/ring.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct DvbcTelemetry {
    // ---- the part every mode has
    uint64_t seq = 0;
    int state = 0;               // 0 searching, 1 equaliser / carrier converging, 2 locked and decoding
    double cfoHz = 0;
    float snrDb = 0;             // MER of the decided symbols
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // Reed-Solomon packets

    // ---- DVB-C
    bool annexB = false;         // J.83 annex B (clear QAM)
    DocsisStatus docsis;         // DOCSIS management, when this carrier is a cable modem downstream
    int qam = -1;                // 0..4: 16, 32, 64, 128, 256-QAM, -1 not known yet
    bool qamManual = false;
    double symbolRate = 0;       // Hz, in use (timing loop included)
    bool symbolRateManual = false;
    double symbolRateMeasured = 0;   // from the spectral line of the squared envelope (0: not measured)
    double width3dBHz = 0;           // -3 dB width of the carrier
    float rollOff = 0.15f;
    bool annexC = false;
    bool inverted = false;
    double clockPpm = 0;         // symbol clock against the nominal rate
    float levelDbfs = -120;
    float merDb = 0;
    double preRsByteErr = 0;     // bytes Reed-Solomon changed, as a fraction
    uint64_t rsClean = 0, rsCorrected = 0, rsFailed = 0;
    uint64_t packets = 0, packetsBad = 0;
    double netMbps = 0;
    bool lockSpectrum = false, lockTiming = false, lockEq = false, lockCarrier = false, lockSync = false, tsLock = false;
    std::string stage;           // plain text of what the receiver does now
    std::string note;            // "the manual symbol rate did not fit: using the measured 6.952 Msym/s" ...
    uint64_t reacquisitions = 0;
    uint64_t dropped = 0;        // input samples lost because the decoder fell behind

    // pictures
    std::vector<cf32> cells;     // recent symbols after equaliser and carrier loop, unit mean power, at most 2048
    std::vector<float> psdDb;    // spectrum of the input, 0 dB at the peak, fft shifted over the input rate
    double psdSpanHz = 0;
    std::vector<float> eqDb;     // equaliser tap magnitudes (dB, the strongest 0), half-symbol spacing
    int eqCentre = 0;            // index of the main tap in eqDb
    std::vector<float> merHistory;
};

inline std::string dvbcSummary(const DvbcTelemetry& t) {
    static const char* const q[] = {"16-QAM", "32-QAM", "64-QAM", "128-QAM", "256-QAM"};
    char b[240];
    snprintf(b, sizeof b, "DVB-C%s %s  %.4f Msym/s  MER %.1f dB  CFO %+.0f Hz  RS ok %llu bad %llu  packets %llu%s%s", t.annexC ? " (annex C)" : "",
             t.qam >= 0 && t.qam < 5 ? q[t.qam] : "QAM ?", t.symbolRate / 1e6, t.merDb, t.cfoHz, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
             (unsigned long long)t.packets, t.tsLock ? "  TS lock" : "", t.inverted ? "  inverted" : "");
    return b;
}

} // namespace dect2
