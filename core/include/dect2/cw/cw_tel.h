// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CW (Morse) receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One signal of the skimmer: a carrier found in the window, its own filter, envelope detector and timing decoder
struct CwSignal {
    int id = 0;                  // stable while the signal is followed
    double offsetHz = 0;         // from the dial frequency (the signal is heard at dial + offsetHz)
    float wpm = 0;               // 0 until the speed is known
    float snrDb = 0;             // mark level over the noise in the signal's 100 Hz filter
    bool key = false;            // key down now
    bool locked = false;         // the speed is known: text is coming
    bool farnsworth = false;     // the gaps are longer than the characters' speed asks (Farnsworth spacing)
    bool manual = false;         // picked by the user (a click on the waterfall), kept while it is the focus
    double lastActiveSec = 0;    // signal seconds of the last key-down
    double firstSec = 0;
    uint64_t chars = 0;          // characters decoded
    std::string text;            // the newest kCwTextMax characters
    std::vector<std::string> calls;   // callsigns in the text, newest last
};

constexpr size_t kCwTextMax = 600;
constexpr int kCwMaxSignals = 24;

struct CwTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 nothing heard, 1 signals in the window, 2 decoding text
    double cfoHz = 0;            // not measured (CW is heard where it is): always 0
    float snrDb = 0;             // of the focused (else the strongest) signal
    float levelDb = -200;        // input power, dBFS
    bool dataValid = false;      // some signal is decoding
    uint64_t blocksOk = 0;       // characters decoded
    uint64_t blocksBad = 0;      // characters not in the table
    // details
    double timeSec = 0;
    double inputRate = 0;
    double windowHz = 0;         // the skimmer covers -windowHz .. +windowHz around the dial frequency
    float floorDb = -200;        // the noise floor of the window spectrum, dB per bin
    int focusId = -1;            // the signal of the big text view (-1 none)
    std::vector<CwSignal> signals;     // by frequency
    // the window spectrum (dB per bin, -windowHz .. +windowHz), newest, for the waterfall
    std::vector<float> spectrum;
    uint64_t specSeq = 0;        // counts the spectrum rows
};

inline const CwSignal* cwFocus(const CwTelemetry& t) {
    for (const auto& s : t.signals) if (s.id == t.focusId) return &s;
    return nullptr;
}

inline std::string cwSummary(const CwTelemetry& t) {
    char b[200];
    int dec = 0;
    for (const auto& s : t.signals) if (s.locked) dec++;
    if (t.signals.empty()) return "searching for CW";
    const CwSignal* f = cwFocus(t);
    if (f && f->locked) snprintf(b, sizeof b, "%zu signals, %d decoding; focus %+.0f Hz, %.0f WPM", t.signals.size(), dec, f->offsetHz, f->wpm);
    else snprintf(b, sizeof b, "%zu signals, %d decoding", t.signals.size(), dec);
    return b;
}

} // namespace dect2
