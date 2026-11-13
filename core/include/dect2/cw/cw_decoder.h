// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CW adaptive decoder: key-down / key-up durations in, text with a confidence per character out.
//
// The sender is learned while the text comes: the lengths of the marks and the gaps are modelled in the log domain as classes (dot, dash;
// gap inside a character, between characters, between words) whose means follow the speed (a tracked dot length) plus offsets learned for
// this operator (dash / dot ratio, lazy character gaps, stretched word gaps) and whose spreads are learned too, so a machine, a straight key
// with +-20 % spread and a bug (steady dots, wandering dashes) are all read by the same code. A beam search over the segmentations scores
// every reading by the timing likelihood plus the code validity (the symbol must exist in the Morse table) plus a trigram / word model of
// amateur-radio traffic (cw_lm.h); the characters that the beam agrees on are committed with a confidence (the share of the beam mass that
// read the same) and the best alternative reading.
//
// No allocation after construction: the beam, the event queues and the output queue are fixed arrays.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

namespace dect2 {
namespace cw {

constexpr int kFistBins = 40;       // histogram bins: ln(duration / dot) from -1.6 to +2.8

struct DecTok {
    char text[8] = {};               // "A", "5", "<AR>", " " (word space), "*" (not in the table)
    uint8_t conf = 100;              // 0 - 100
    char alt[8] = {};                // the best other reading of this character (empty: none worth showing)
};

struct FistStats {
    bool locked = false;
    float wpm = 0, dotMs = 0;
    float dashRatio = 3;             // dash / dot, learned
    float charGapDots = 3, wordGapDots = 7, intraGapDots = 1;
    float sigDot = 0, sigDash = 0, sigChar = 0, sigWord = 0;   // spreads in the log domain (0.1 = 10 %)
    float quality = 0;               // 0..1: how steady the keying is (1 = a machine)
    float fit = 0;                   // the mean squared z of the marks: ~1 when the model fits
    float marks[kFistBins] = {};     // dot / dash histogram in ln(ms / dot), newest weigh more
    float gaps[kFistBins] = {};
};

// tuning knobs (the benchmark sweeps them; the defaults are the shipped values)
struct FistTune {
    float lmWeight = 0.4f;           // weight of the character model against the timing likelihood
    float wordWeight = 0.7f;         // weight of the word list / callsign bonus
    int delay = 4;                   // characters kept open for a better reading before they are committed
    float alphaL = 0.10f;            // speed tracking rate per mark
    float tailW = 0.003f, tailMul = 3.0f, tailBase = 0.15f;   // the broad tail of a class (outliers): share, spread = mul * sigma + base
    float alphaC = 0.04f;            // learning rate of the class offsets and spreads (after the start)
};
FistTune& fistTune();

class FistDecoder {
public:
    FistDecoder();
    ~FistDecoder();
    void reset();
    void setManualWpm(double wpm);   // 0: automatic (the default); else the dot length is fixed (the ratio and the gaps are still learned)
    void mark(double ms);            // a key-down that just ended
    void space(double ms);           // a key-up that just ended
    void idle(double ms);            // the key is up and has been for ms: ends the character and the word in time
    size_t pop(DecTok* out, size_t max);   // the characters committed since the last call
    bool locked() const;
    double dotMs() const;
    double wpm() const;
    double dashRatio() const;
    void stats(FistStats& s) const;
    // the readings of the text not committed yet, best first; returns how many (<= maxN), each at most 47 characters
    int liveAlternatives(char (*out)[48], int maxN) const;

    struct Impl;
private:
    std::unique_ptr<Impl> p_;
    FistDecoder(const FistDecoder&) = delete;
    FistDecoder& operator=(const FistDecoder&) = delete;
};

} // namespace cw
} // namespace dect2
