// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radio astronomy (hydrogen line) maths: frequency <-> radial velocity, DC spike removal, on - off / off, frequency-switch folding,
// polynomial baseline flattening outside a marked line window, smoothing, the peak, and the CSV format of a saved spectrum.
// Pure functions on power spectra (linear power per bin, lowest frequency first, centre bin = the radio's frequency).
#pragma once
#include <string>
#include <vector>

namespace dect2 {
namespace astro {

constexpr double kC = 299792.458;          // km/s
constexpr double kHiHz = 1420.405751e6;    // neutral hydrogen, 21 cm

struct Line { const char* name; double restHz; };
const Line* lines(int& count);             // H I and the four OH lines (the "custom" entry is the app's)

// radio convention, topocentric (no LSR correction): v = c (f0 - f) / f0, positive = receding
inline double velocityKms(double hz, double restHz) { return kC * (restHz - hz) / restHz; }
inline double freqForVelocity(double kms, double restHz) { return restHz * (1 - kms / kC); }

// bins centre - half .. centre + half replaced by a straight line between their neighbours
void removeDc(std::vector<double>& p, int half);

// least-squares polynomial (degree 0 .. 9) through y[i] where use[i]; returns the fitted curve at every bin (x scaled to -1 .. 1)
std::vector<double> polyBaseline(const std::vector<double>& y, const std::vector<bool>& use, int degree);

// boxcar of width w bins (1 = none), edges shortened
std::vector<double> smooth(const std::vector<double>& y, int w);

struct Params {
    double centerHz = kHiHz;    // the radio's frequency (the centre bin)
    double rate = 0;            // sample rate = span, Hz
    double restHz = kHiHz;      // rest frequency the velocity is counted from
    int dcHalf = 2;             // DC spike: bins each side of the centre bridged (-1 off)
    int degree = 3;             // baseline polynomial (-1 none)
    bool window = true;         // exclude (and search) the line window
    double winLoKms = -150, winHiKms = 150;
    double edge = 0.08;         // fraction of the band at each edge left out of the fit and the peak search (filter roll-off)
    int smoothBins = 1;
    // frequency switching: off = the same sky with the radio shifted by switchHz; fold puts the negative image back onto the line
    double switchHz = 0;
    bool fold = true;
};

struct Result {
    std::vector<double> freqHz, velKms;   // per bin
    std::vector<double> ratio;            // the processed spectrum: (on - off) / off, or on / median - 1 without a reference; baseline removed
    std::vector<double> baseline;         // the fitted baseline (same units, before removal)
    std::vector<bool> inWindow;
    int peak = -1;                        // the highest bin; peakHz / peakKms: the centroid of the part above half of it
    double peakHz = 0, peakKms = 0, peakRatio = 0, rms = 0, snr = 0;
};

// on: the integrated spectrum; off: the reference (cold sky / off source) or the frequency-switched phase, or empty
Result process(const std::vector<double>& on, const std::vector<double>& off, const Params& p);

// CSV: "# " comment lines (centre, rest, rate, fft, integration), a header, then freq_hz,velocity_kms,power_db,ratio,baseline per bin
std::string toCsv(const Result& r, const std::vector<double>& on, const Params& p, double integrationSec);
struct CsvSpectrum { double centerHz = 0, restHz = 0, rate = 0, integrationSec = 0; std::vector<double> freqHz, velKms, powerDb, ratio, baseline; };
bool fromCsv(const std::string& text, CsvSpectrum& out);

} // namespace astro
} // namespace dect2
