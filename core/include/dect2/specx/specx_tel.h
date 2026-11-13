// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// What the spectrum explorer reports (RxTelemetry::specx, standard 42).
#pragma once
#include <cstdint>
#include <vector>

namespace dect2 {

struct SpecxTelemetry {
    uint64_t seq = 0;
    int state = 0;                 // 0 waiting for samples, 1 showing the spectrum
    double rate = 0;               // input sample rate, Hz
    int fftSize = 0;               // bins of the spectrum (RBW = rate / fftSize)
    double enbw = 1;               // noise bandwidth of the window, in bins
    int averaging = 1;             // spectra averaged (exponential, this many)
    uint64_t frames = 0;           // spectra taken since the last reset (retune): a sweep waits for a few
    std::vector<float> spectrum;   // dBFS per bin, lowest frequency first, centre bin = the radio's frequency
    float floorDb = -200, peakDb = -200;
    double peakOffsetHz = 0;       // of the strongest bin, from the centre
    double powerDb = -200;         // total input power, dBFS
};

} // namespace dect2
