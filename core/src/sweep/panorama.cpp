// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/sweep/panorama.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace sweep {

SweepConfig panoramaConfig(double centreHz, double spanHz) {
    SweepConfig c;
    spanHz = std::max(1e5, spanHz);
    c.startHz = std::max(1e3, centreHz - spanHz / 2);
    c.stopHz = c.startHz + spanHz;
    c.binHz = spanHz / 1000.0;
    c.dwellSec = 0.004;
    c.settleSec = 0.003;
    c.cropFrac = 0.10;
    c.dcSkip = true;
    c.minIntervalSec = 0;
    c.maxSweeps = 0;
    return c;
}

std::vector<float> panoramaColumns(const Sweep& s, int cols) {
    std::vector<float> out((size_t)std::max(0, cols), -150.f);
    const size_t n = s.bins();
    if (cols <= 0 || n == 0) return out;
    for (size_t i = 0; i < n; i++) {
        float& o = out[std::min<size_t>((size_t)cols - 1, i * (size_t)cols / n)];
        o = std::max(o, s.db[i]);
    }
    return out;
}

int panoramaSteps(double spanHz, double rateHz) {
    if (rateHz <= 0) return 0;
    SweepConfig c = panoramaConfig(100e6, spanHz);
    return (int)planHops(c, rateHz).centers.size();
}

} // namespace sweep
} // namespace dect2
