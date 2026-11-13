// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tone decoder telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kTonesEventMax = 300;

enum ToneKind { TkCtcss = 0, TkDcs, TkDtmf, TkSeq, TkSelcal, TkTwoTone, TkKinds };

// One detection (a sub-audible tone or code that came and went, a key press string, a call)
struct ToneEvent {
    int64_t wall = 0;               // computer clock (UTC seconds) when it began
    double timeSec = 0;             // signal seconds
    int kind = 0;                   // ToneKind
    std::string text;               // "100.0 Hz", "D023N", "1234#", "12345", "AB-CD", "1000.0 + 1500.0 Hz"
    std::string detail;             // standard, tone length, the other readings
    double durSec = 0;              // how long it lasted so far
    float levelDb = -200;
    bool active = false;            // still present
};

struct TonesTelemetry {
    uint64_t seq = 0;
    int state = 0;                  // 0 nothing, 1 a signal (audio) present, 2 something was decoded in the last minute
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0;          // detections
    uint64_t blocksBad = 0;
    // details
    double timeSec = 0, inputRate = 0;
    bool audioInput = false;        // real audio samples (sound card / recording), not a radio's FM
    int demod = 0;                  // the radio's channel: TonesDemod (tones_rx.h), 0 FM
    float audioDb = -200;           // the audio, rms, 0 dB = 5 kHz deviation (or full scale for audio input)
    // sub-audible
    int ctcssIdx = -1;              // locked tone (index into tones::ctcssHz), -1 none
    double ctcssHz = 0;             // measured
    float ctcssDb = -200;           // tone level, 0 dB = full scale of the audio
    int dcsCode = -1;               // locked code as written (23 for 023), -1 none
    bool dcsInv = false;
    bool dcsStd = false;
    std::string dcsAlias;           // "D047I": the same waveform read the other way
    std::vector<float> ctcssPow;    // the 50 tones, dB, newest analysis
    // voice band
    double toneHz = 0;              // the strongest pure tone now (0 none)
    float toneDb = -200;
    std::vector<float> spec;        // audio spectrum 0 .. 3.5 kHz, dB, 112 bins
    std::string dtmfString;         // the digits of the key press group in progress
    // gate
    int gateMode = 0;               // 0 open always, 1 open on any tone / code, 2 open on the chosen one
    bool gateOpen = true;
    int counts[TkKinds] = {};
    std::vector<ToneEvent> events;  // newest first
};

inline const char* toneKindName(int k) {
    static const char* n[TkKinds] = {"CTCSS", "DCS", "DTMF", "Selcall", "SELCAL", "2-tone"};
    return n[k < 0 || k >= TkKinds ? 0 : k];
}

inline std::string tonesSummary(const TonesTelemetry& t) {
    char b[160];
    if (t.ctcssIdx >= 0) { snprintf(b, sizeof b, "CTCSS %.1f Hz", t.ctcssHz); return b; }
    if (t.dcsCode >= 0) { snprintf(b, sizeof b, "DCS D%03d%c", t.dcsCode, t.dcsInv ? 'I' : 'N'); return b; }
    if (!t.events.empty() && t.dataValid) { snprintf(b, sizeof b, "%s %s", toneKindName(t.events[0].kind), t.events[0].text.c_str()); return b; }
    snprintf(b, sizeof b, "listening for tones, %llu heard", (unsigned long long)t.blocksOk);
    return b;
}

} // namespace dect2
