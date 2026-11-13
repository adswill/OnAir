// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// What the radio astronomy tool reports (RxTelemetry::astro, standard 49).
#pragma once
#include <cstdint>
#include <vector>

namespace dect2 {

struct AstroTelemetry {
    uint64_t seq = 0;
    int state = 0;                 // 0 waiting for samples, 1 integrating
    double rate = 0;               // input sample rate, Hz (= span)
    int fftSize = 0;
    double integrationSec = 0;     // the chosen integration time
    double doneSec = 0;            // signal integrated so far in the running average (seconds of samples)
    float progress = 0;            // doneSec / integrationSec
    uint64_t frames = 0;           // FFTs in the running average
    uint64_t completed = 0;        // integrations finished since the last reset (each one ends up in `last`)
    std::vector<double> avg;       // running average, linear power per bin, lowest frequency first, centre bin = the radio's frequency
    std::vector<double> last;      // the last finished integration (empty until one is)
    // frequency switching: the second phase, the radio shifted by switchHz (empty when not switching)
    bool switching = false;
    double switchHz = 0;
    std::vector<double> avgOff, lastOff;
    double powerDb = -200;         // total input power, dBFS
};

} // namespace dect2
