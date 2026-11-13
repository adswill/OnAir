// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Local display / HDMI emission: a monitor, a TV or its cable leaks the TMDS pixel clock (HDMI, DVI) and the picture data it carries.
// What the radio then sees: a clean line at the pixel clock (25.175, 74.25, 148.5, 297, 594 MHz ...) and at its harmonics, and around them a
// comb of lines spaced by the line rate (pixel clock / total pixels per line, 27 - 135 kHz), the "parallel lines" on a waterfall that do not
// move. This tells such emissions from the radio's own LO or clock leak and estimates the video timing behind them: the pixel clock from
// the harmonics, and the line rate from the comb, which settles the total pixels per line and so the resolution and refresh rate.
// The table is CEA-861 (HDMI TV formats, also the 1/1.001 rates) and VESA DMT / CVT-RB (computer monitors). Rules of thumb, not a
// measurement of the display: the result names the likeliest timings with a confidence, alternatives after it (docs/HDMI-EMISSION-3.9.md).
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace dispem {

// "Local display / HDMI emission, not your SDR" (English; the app translates it)
const char* label();

struct Timing {
    const char* name;          // "1920x1080 60 Hz"
    const char* standard;      // "CEA-861 VIC 16", "VESA DMT", "CVT-RB"
    int width, height;
    double refreshHz;          // nominal (the field rate for interlaced formats)
    bool interlaced;
    double pclkHz;             // pixel clock
    int htotal, vtotal;        // total pixels per line, total lines per frame
    double lineRateHz() const { return pclkHz / htotal; }
};
const std::vector<Timing>& timings();

struct Line { double hz = 0; float levelDb = 0; };   // a narrow line: RF Hz, dB above its surroundings

struct Match {
    int timing = -1;           // index into timings()
    double pclkHz = 0;         // the pixel clock measured (from the harmonics found)
    double ppm = 0;            // its offset from the table's (the display's crystal and the radio's together)
    std::vector<int> harmonics;        // harmonic numbers found
    int expected = 0;          // harmonics of this clock inside the span
    int combLines = 0;         // lines found on the line-rate comb around the harmonics
    double combHz = 0;         // the comb spacing measured (0 none)
    double score = 0;
    double confidence = 0;     // 0..1: this timing (the share among the matches times how sure it is a display at all)
};

struct Result {
    bool found = false;
    double confidence = 0;     // 0..1: it is a display / HDMI emission (any timing)
    std::vector<Match> matches;   // best first: the likely timing, then the alternatives (same clock, other line rates; other clocks)
    double pclkHz = 0;         // the best match's measured pixel clock
    bool tableMatch = false;   // the clock is a standard one (false: a display-like comb of harmonics at a clock not in the table)
    std::vector<double> members;  // RF Hz of the lines that belong to it (harmonics and comb lines)
    std::vector<std::string> evidence;   // one line per rule: "harmonics 1, 2, 3 of 148.5000 MHz (+12 ppm)"
    std::string summary;       // "1920x1080 60 Hz (CEA-861 VIC 16), pixel clock 148.5000 MHz, 84 %"
};

struct Config {
    double tolPpm = 400;       // how far a clock may be from the table's (display crystal + radio error)
    int maxHarmonic = 30;      // harmonics looked at
    double minLevelDb = 6;     // lines weaker than this over their surroundings are ignored
    int maxComb = 12;          // comb lines looked for on each side of a harmonic
};

// Narrow lines of a spectrum (dB per bin, bin k at f0Hz + k * binHz): local peaks thrDb over the local median, at most maxLines, strongest first
std::vector<Line> findLines(const float* db, int n, double f0Hz, double binHz, double thrDb = 8, size_t maxLines = 400);
// The analysis on lines the caller found; [spanLoHz, spanHiHz] the RF range that was looked at, binHz the resolution
Result analyse(const std::vector<Line>& lines, double spanLoHz, double spanHiHz, double binHz, const Config& c = Config());
Result analyseSpectrum(const float* db, int n, double f0Hz, double binHz, const Config& c = Config());
// From complex samples (rfCentreHz: the RF of the samples' centre): an averaged spectrum fine enough to see the comb (at most 1 M samples used)
Result analyseIq(const cf32* x, size_t n, double rateHz, double rfCentreHz, const Config& c = Config());
std::string toText(const Result& r);   // the result as English text, for the identifier and the clipboard

// ---- generators (tests, the demo) ----
// Samples of a display's emission as the radio sees it at rfCentreHz: the pixel clock harmonics inside the band and, around each, the comb of
// a static picture's data repeating every line (random phases and levels falling off with distance), the frame structure on top, noise.
// levelDb: the clock line over the noise in one bin of a 65536-point FFT.
struct GenConfig {
    int timing = 0;            // index into timings()
    double rateHz = 2e6, rfCentreHz = 148.5e6;
    double seconds = 0.25;
    double ppm = 0;            // clock offset
    float levelDb = 40;        // clock line over the noise (per 65536-point bin)
    float combDb = -14;        // comb lines next to the clock, relative to the clock line
    uint64_t seed = 1;
};
std::vector<cf32> generateIq(const GenConfig& g);
// A wide spectrum (dB per bin) with the harmonics of the timing's pixel clock (odd ones stronger, levels falling with frequency) over a floor
std::vector<float> simulateSpectrum(int timing, double f0Hz, double binHz, int n, double ppm, float floorDb, float levelDb, uint64_t seed);
int findTiming(const char* name);      // index of a timing by name ("1920x1080 60 Hz"), -1 none

} // namespace dispem
} // namespace dect2
