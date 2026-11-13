// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// What the ionospheric sounder mode reports (RxTelemetry::iono, standard 53).
#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

// ---- scaling (iono_scale.h): echo traces and the standard parameters of the latest sweep of a sounder

// An echo trace found in an ionogram: a run of delay peaks across frequency
struct IonoTracePt { float fMhz = 0, rangeKm = 0, db = 0; };
struct IonoTrace {
    enum Layer : uint8_t { Unknown = 0, E = 1, F1 = 2, F2 = 3, Ground = 4 };
    uint8_t layer = Unknown;    // vertical: the layer; oblique: E or F2 (F) of the mode
    uint8_t hops = 1;           // vertical: 2 = a multiple reflection (ground - layer twice); oblique: the hops of the mode
    bool x = false;             // the extraordinary (X) trace (vertical, where told apart)
    bool highRay = false;       // oblique: the high (Pedersen) ray above the nose
    std::vector<IonoTracePt> pts;   // ascending frequency, one per 50 kHz column
};

// One propagation mode of an oblique path (1E, 1F, 2F, ...)
struct IonoMode { uint8_t layer = IonoTrace::F2; uint8_t hops = 1; double mufMhz = NAN; bool mufAbove = false; double minRangeKm = NAN; };

// Scaled values of one sweep. NAN = not scaled; *Above: only a lower bound (the trace ran to the edge of the captured span; URSI "D")
struct IonoScaling {
    int geometry = 0;           // 0 nothing scaled, 1 vertical (a local ionosonde), 2 oblique (a distant sounder)
    double t = 0;               // the sweep start it comes from
    // vertical
    double foE = NAN, foF1 = NAN, foF2 = NAN, fxF2 = NAN;   // MHz
    double hE = NAN, hF = NAN, hF2 = NAN, hmF2 = NAN;      // km: h'E, h'F (lowest of the F traces), h'F2, peak height (h'(0.834 foF2))
    double m3000 = NAN, muf3000 = NAN;                     // M(3000)F2 and MUF(3000)F2 (MHz)
    double fmin = NAN;                                     // the lowest echo, MHz
    bool foF2Above = false, foEAbove = false;
    // oblique
    double distKm = NAN;        // ground distance
    int distFrom = 0;           // 0 unknown, 1 from the group delay, 2 from the known sounder and the receiver location
    double pathMuf = NAN;       // MHz (the nose of the highest-MUF mode)
    bool pathMufAbove = false;
    std::vector<IonoMode> modes;
    // identification (iono::knownSounders())
    int known = -1;             // index in the list, -1 none
    int knownMatch = 0;         // 0 none, 1 the rate (and the distance, with a receiver location), 2 rate and schedule
    int candidates = 0;         // listed sounders of this rate
    int traces = 0;             // echo traces used
};

// The history of the scaled values of a sounder, one point per sweep that gave values
struct IonoHistPt {
    double t = 0;
    int geometry = 0;
    float foE = NAN, foF1 = NAN, foF2 = NAN, hE = NAN, hF = NAN, m3000 = NAN, muf3000 = NAN, pathMuf = NAN, distKm = NAN;
};

// A chirp sounder (ionosonde or oblique sounder) heard at least once: the sweep rate, and once two sweeps were seen the repetition period
// and the start offset in it (sweep start, the moment the sweep passes 0 MHz, modulo the period).
struct IonoSounder {
    int id = 0;
    double rateHz = 0;          // sweep rate, Hz/s (100e3 = 100 kHz/s)
    double periodSec = 0;       // 0 until two sweeps were seen
    double offsetSec = 0;       // lastT0 modulo the period (with a radio: seconds after the UTC epoch modulo the period)
    double lastT0 = 0;          // start of the last sweep (time base of IonoTelemetry::nowSec)
    double lastSeen = 0;        // when it was last in the captured span
    double nextT0 = 0;          // the next sweep start expected (0 = period unknown)
    double snrDb = 0;           // strength of the last sweep (chirp peak over the noise, per detector bin)
    double fLoHz = 0, fHiHz = 0;   // the part of 0 .. 30 MHz its sweeps were heard in
    int sweeps = 0;             // sweeps heard
    int missed = 0;             // expected sweeps that did not come
    bool active = false;        // a sweep of it is in the span now
    IonoScaling scale;          // of the latest sweep that gave traces (geometry 0: none yet)
};

// One sweep heard (the timeline)
struct IonoEvent {
    double t0 = 0;              // sweep start
    int sounder = 0;            // IonoSounder::id
    double rateHz = 0;
    double fLoHz = 0, fHiHz = 0;   // the part of the sweep that crossed the captured span
    double snrDb = 0;
    bool predicted = false;     // the sweep was expected from the period (its whole crossing went into the ionogram)
};

struct IonoTelemetry {
    uint64_t seq = 0;
    int state = 0;              // 0 waiting for samples, 1 searching
    double rate = 0;            // input sample rate, Hz
    double centerHz = 0;        // the radio's frequency
    double nowSec = 0;          // stream time (with a radio: UTC seconds, from the computer clock; a file or the test signal: from 0)
    bool locked = true;         // sweeps start on whole seconds (GPS-timed sounders): delays are absolute, else relative to the first echo
    double spanLoHz = 0, spanHiHz = 0;   // the usable captured span now
    std::vector<uint8_t> covered;        // kCols: the columns of 0 .. 30 MHz the radio has captured since the reset
    std::vector<IonoSounder> sounders;
    std::vector<IonoEvent> timeline;     // newest last
    int sel = -1;                        // index in sounders of the ionogram below (-1: none yet)
    std::vector<float> ionoDb;           // kRows x kCols (row = delay bin, row 0 = 0 ms), dB over each column's median; NAN where never swept
    std::vector<uint8_t> swept;          // kCols: columns of the selected sounder with data
    std::vector<IonoTrace> traces;       // the selected sounder's traces of its latest scaled sweep (IonoSounder::scale)
    std::vector<IonoHistPt> history;     // the selected sounder's scaled values over time, oldest first
    bool haveObserver = false;           // the receiver location is set (distances of known sounders)
    double obsLat = 0, obsLon = 0;
    int activeSweeps = 0;
    uint64_t frames = 0, hits = 0;       // detector FFTs, chirp peaks found
    int fftSize = 0;                     // detector FFT
    double powerDb = -200;               // input power, dBFS
};

} // namespace dect2
