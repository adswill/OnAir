// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CODAR / HF ocean radar monitor (SeaSonde, WERA and similar coastal FMCW radars in the ITU oceanographic bands): receive only. These are
// broadcast radar sweeps: nothing is decrypted and nothing is transmitted.
//
// Detector: a Hann FFT of about 1 ms (N = sample rate / 1000, a power of two) every N/2 samples. Each bin is compared with a slow
// per-bin noise floor; the strongest narrow peaks of a frame are linked from frame to frame into tracks (a chirp moves a fraction of a bin
// per frame). A track that ends is fitted with a line: its slope is the sweep rate (and direction), its extent the sweep width and its
// centre the sweep centre. Tracks that are not linear, too narrow, too slow or wider than 500 kHz are not counted (carriers, other chirps
// such as ionosondes). Sweeps with the same centre and width are one station; the interval between their starts is the repetition.
// ID: the power inside a station's sweep width is followed over time (the whole width keys on and off together); on / off runs go through the
// CW timing decoder (cw_morse.h) and a word of 4 or more characters seen at least twice is reported, always labelled unverified (the keying
// is built from the published description of the ID, not from a recording).
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct CodarTelemetry;   // codar_tel.h

class CodarReceiver {
public:
    CodarReceiver();
    ~CodarReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);          // the radio's centre: absolute frequencies and the band are worked out from it (0: unknown)
    bool ready() const;
    void reset();                         // the detector only: the stations stay
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(CodarTelemetry& out, uint64_t lastSeq);
    // The stations and events stay through reset(), configure() and retunes: only clearResults() empties them
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning codarTuning();

namespace codar {

struct Band { const char* name; double loMhz, hiMhz; };
const Band* bands(int& count);                           // 4.4 - 4.65 (4.438 - 4.488 listed), 5.25, 12 - 14, 24.45 - 24.65, 41.5 - 42.5 MHz
std::string bandOf(double mhz);                          // the band's name, "" outside them
// Radio centres (MHz) that together take in every radar band at this sample rate (each covers 80% of the rate), band by band; a radio wide
// enough for a whole band needs one step per band
std::vector<double> scanPlan(double sampleRate);
std::string hintFor(double centreMhz, double bwKhz, double periodSec);   // "possible ...": a guess from the numbers, never an identification

} // namespace codar
} // namespace dect2
