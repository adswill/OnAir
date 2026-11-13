// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tuning inside an IQ recording (tests/app/test_file_vfo.cpp). A file cannot retune a radio: the spectrum stays on the recording's centre
// frequency and rate, and the tune box (the VFO) only moves the demodulator's offset from that centre. The VFO frequency shown is
// centre + offset. These are the pure rules: the mapping, the clamp that keeps the passband inside the recorded span, the edge warning, the DC
// spike position, and the centre frequency a file name may carry.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
namespace filevfo {
// the part of the span the engine's anti-alias filter keeps (the same 0.45 of the rate the second mode and the wide receivers use)
inline double usableHalfSpanHz(double rateHz) { return 0.45 * std::max(0.0, rateHz); }
// the farthest the VFO's centre may sit from the recording's centre with a passband bwHz wide inside the usable span (0 when it does not fit)
inline double maxOffsetHz(double rateHz, double bwHz) { return std::max(0.0, usableHalfSpanHz(rateHz) - std::max(0.0, bwHz) / 2); }
inline double clampOffsetHz(double offsetHz, double rateHz, double bwHz) {
    const double m = maxOffsetHz(rateHz, bwHz);
    return std::min(m, std::max(-m, offsetHz));
}
inline double vfoMhz(double centreMhz, double offsetHz) { return centreMhz + offsetHz / 1e6; }
inline double offsetHzFromVfo(double vfoMhzV, double centreMhz) { return (vfoMhzV - centreMhz) * 1e6; }
struct Plan {
    double offsetHz = 0;   // the offset to use (clamped)
    double vfoMhz = 0;     // centre + offset
    bool clamped = false;  // the asked frequency lay beyond what the passband allows
    bool nearEdge = false; // the passband touches (within 3 % of the span) the edge of the usable band
    bool tooWide = false;  // the passband alone is wider than the usable span: no offset fits
};
// What a click / drag / typed frequency (askedMhz) does in a recording centred on centreMhz at rateHz with a passband of bwHz
inline Plan plan(double askedMhz, double centreMhz, double rateHz, double bwHz) {
    Plan p;
    const double want = offsetHzFromVfo(askedMhz, centreMhz);
    p.offsetHz = clampOffsetHz(want, rateHz, bwHz);
    p.clamped = std::fabs(want - p.offsetHz) > 0.5;
    p.vfoMhz = vfoMhz(centreMhz, p.offsetHz);
    p.tooWide = rateHz > 0 && bwHz >= 2 * usableHalfSpanHz(rateHz);
    p.nearEdge = rateHz > 0 && (p.tooWide || std::fabs(p.offsetHz) >= maxOffsetHz(rateHz, bwHz) - 0.03 * rateHz);
    return p;
}
// The recording's DC spike / LO leakage sits at its centre: relative to the VFO it is -offset. True when it lies inside the passband (plus
// guardHz for the spike's own width): the demodulator then hears it, and the notch toggle is worth a try.
inline bool spikeInPassband(double offsetHz, double bwHz, double guardHz) { return std::fabs(offsetHz) < bwHz / 2 + std::max(0.0, guardHz); }
// "x_554.000MHz_10Msps.cs8", "capture-100.5mhz", "1.09GHz", "uat_978000khz": the centre frequency the file name says, in Hz; 0 if it does not.
// "Msps" / "ksps" are rates, not frequencies, and are skipped.
inline double guessCentreHz(const std::string& path) {
    std::string n = path;
    const size_t sl = n.find_last_of("/\\");
    if (sl != std::string::npos) n = n.substr(sl + 1);
    for (auto& c : n) c = (char)std::tolower((unsigned char)c);
    static const struct { const char* u; double mul; } units[] = {{"ghz", 1e9}, {"mhz", 1e6}, {"khz", 1e3}};
    for (const auto& u : units) {
        size_t p = n.find(u.u);
        while (p != std::string::npos) {
            size_t b = p;
            while (b > 0 && (std::isdigit((unsigned char)n[b - 1]) || n[b - 1] == '.')) b--;
            if (b < p) {
                const double v = std::atof(n.substr(b, p - b).c_str()) * u.mul;
                if (v >= 1e3 && v <= 3e11) return v;
            }
            p = n.find(u.u, p + 1);
        }
    }
    return 0;
}
}
