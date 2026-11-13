// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// PACTOR-II / -III / -IV: demodulation and framing of the physical layer (receive only).
//
// What is public (SCS technical descriptions, Wavecom decoder notes, see docs/pactorhf.md): PACTOR-II is two D-PSK carriers 200 Hz apart at 100 baud,
// PACTOR-III is 2 .. 18 D-PSK carriers at 480 + 120 n Hz, 100 baud, PACTOR-IV is one PSK carrier at 1500 Hz, 1800 baud (levels 5 .. 10; levels 1 .. 4
// are chirp or spread spectrum at 66.7 .. 225 baud). The channel coding (convolutional code, puncturing, interleaver, scrambler, header layout) is
// proprietary and not published: this module finds the carriers, recovers the symbols (differential phase), the symbol clock and the repeating frame
// structure, and reports the symbols. It does not produce text.
//
// All functions work on 8 kHz real audio (the upper sideband, as the other HF modes do).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 { namespace pactorhf {

constexpr double kFs = 8000;

struct Carrier {
    double hz = 0;               // frequency after the fine search
    double lineDb = 0;           // strength of the 100 baud clock line of this carrier against the noise
    int order = 0;               // 2 (D-BPSK), 4 (D-QPSK), 8, 16 : phase steps per symbol
    double r = 0;                // concentration of the phase steps (1 = perfect)
    std::vector<uint8_t> sym;    // the phase step index of every symbol (0 .. order-1), the same symbol clock for all carriers
};

struct FrameInfo {
    bool found = false;
    int period = 0;              // symbols from one repeat to the next
    double periodSec = 0;
    int syncPos = 0, syncLen = 0;   // constant part of the frame (symbols), position in the frame
    std::string syncHex;         // that part as bytes (carrier 0 symbols, 2 bits each), for the log
    int copies = 0;
    double match = 0;            // agreement of two neighbouring frames (chance: 1 / order)
};

struct Analysis {
    bool found = false;
    int variant = 0;             // 2, 3, 4 : what the signal looks like
    double baud = 0;
    double centreHz = 0, spacingHz = 0, bwHz = 0;
    double snrDb = 0;            // clock line / noise
    double clockPpm = 0;         // symbol clock against 100 baud (or 1800)
    double startSec = 0, lengthSec = 0;
    int nCarriers = 0;
    int level = 0;               // PACTOR-III speed level from the carrier count (1 .. 6), 0 unknown
    std::vector<Carrier> car;
    size_t symbols = 0;
    FrameInfo frame;
    int constellation = 0;       // PACTOR-IV: 2 (BPSK), 4, 8, 16 (estimate), 0 unknown
    double lineDb = 0;           // PACTOR-IV: 1800 baud clock line
    std::string note;
};

// Analyse a stretch of audio. variant: 2, 3 or 4 selects what to look for; 0 tries all and returns the strongest.
Analysis analyse(const float* x, size_t n, int variant);

// Pieces, exposed for the tests
// Strength (dB over the floor) of the clock line at `baud` of the envelope of x mixed to 0 Hz around fc (integration over one symbol).
double clockLineDb(const float* x, size_t n, double fc, double baud);
// PACTOR-III speed level for a carrier count, 0 if not a level of the standard
int levelForCarriers(int n);
// 8-PSK etc: bits per symbol
inline int bitsOf(int order) { return order <= 2 ? 1 : order <= 4 ? 2 : order <= 8 ? 3 : 4; }

}} // namespace dect2::pactorhf
