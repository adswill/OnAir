// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Amplitude scintillation index S4 (3.9, docs/SMART5-gnss-3.9.md item 22). Per satellite, from the correlator's signal intensity
// (I^2 + Q^2 of the prompt, or C/N0 in linear units) at 10 - 100 Hz:
//   S4^2 = (<SI^2> - <SI>^2) / <SI>^2 over 60 s, after the slow trend is divided out (a 0.1 Hz low-pass, so elevation and
//   antenna-pattern changes do not count), then the part due to thermal noise is taken off (Van Dierendonck et al. 1993):
//   S4n^2 = (100 / CN0) (1 + 500 / (19 CN0)), CN0 in Hz.
// Flags: S4 >= 0.3 moderate, >= 0.6 strong scintillation (the usual thresholds of ionospheric monitors).
#pragma once
#include <deque>
#include <map>
#include <string>

namespace dect2 {
namespace smart5 {

enum class Scint { None, Weak, Moderate, Strong };

struct S4Result { double s4 = 0, s4Raw = 0, s4Noise = 0; Scint level = Scint::None; bool valid = false; };

class GnssS4 {
public:
    explicit GnssS4(double rateHz = 50, double windowSec = 60) : rate_(rateHz), win_(windowSec) {}
    // intensity: linear power of the prompt correlator; cn0DbHz: the tracking loop's estimate
    void sample(int prn, double intensity, double cn0DbHz);
    S4Result result(int prn) const;
    static const char* levelName(Scint s);
private:
    struct Sat { double trend = 0; bool init = false; double s1 = 0, s2 = 0; int n = 0; double cn0 = 0; S4Result last; };
    std::map<int, Sat> s_;
    double rate_, win_;
};

} // namespace smart5
} // namespace dect2
