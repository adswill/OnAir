// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// One-button Scan / Identify (docs/SCAN-IDENTIFY.md): the beginner's path. proposeAll() finds every signal in a capture (sigid::findRegions), runs
// the existing identifier on each (sigid::identify: bandwidth, symbol rate, modulation, frame syncs, mode fingerprints) and returns one ranked
// proposal per signal, best first: what it probably is, how sure, the runners-up and the Memory that opens it in the right mode. Nothing new is
// measured here; the detectors are the identifier's. proposeOne() is the same for one region (the "I don't know what this is" button).
#pragma once
#include "dect2/channels/memory_bank.h"
#include "dect2/common/ring.h"
#include "dect2/engine/sig_ident.h"
#include <string>
#include <vector>

namespace dect2 {
namespace specx {

enum Sureness { SureNone = 0, SureGuess, SureLikely, SureSure };   // unknown / a guess among several / likely / sure (verified)
const char* surenessName(int s);

struct Guess { std::string name, modeId; double confidence = 0; };

struct Proposal {
    double rfHz = 0, bwHz = 0, snrDb = 0;
    std::string name;             // best guess ("" when nothing fits: the signal is shown as unknown)
    std::string modeId;           // the mode that opens it
    double confidence = 0;        // 0..1 of the best guess
    int sureness = SureNone;
    bool canOpen = false;         // a mode is known for the best guess
    std::string summary;          // measured: "4FSK, 4800 baud, 12.5 kHz wide"
    std::vector<Guess> guesses;   // ranked, at most 4, the best first
    Memory memory;                // opens the best guess (valid when canOpen)
    sigid::Result result;         // the full identifier result (the "Why?" view)
};

struct ScanOptions {
    double rfHz = 0;              // RF of the samples' centre
    size_t maxSignals = 12;
    double thresholdDb = 8;       // a signal must stand this far above the floor
    bool keepUnknown = true;      // list signals nothing fits as "unknown" (the user can still ask for the measurement)
};

Proposal proposeOne(const cf32* x, size_t n, double rateHz, double loHz, double hiHz, double rfHz);   // lo == hi: the strongest signal
std::vector<Proposal> proposeAll(const cf32* x, size_t n, double rateHz, const ScanOptions& o = ScanOptions());

// Beginner words for the verdict: "Sure: DMR", "Probably: NXDN96", "Could be: P25 Phase 1 or NXDN96", "Unknown signal, 12.5 kHz wide"
std::string proposalLine(const Proposal& p);

} // namespace specx
} // namespace dect2
