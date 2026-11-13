// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CODAR / HF ocean radar: the radar bands, the scan plan and the "possible" hint (see codar_rx.h).
#include "dect2/codar/codar_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace codar {

namespace {
// The ITU oceanographic radar bands (docs/codar-research.md): the working edges are a little wide of the allocations
const Band kBands[] = {
    {"4.4 - 4.65 MHz", 4.40, 4.70},
    {"5.25 MHz", 5.20, 5.30},
    {"12 - 14 MHz", 11.90, 14.10},
    {"24.45 - 24.65 MHz", 24.40, 24.70},
    {"41.5 - 42.5 MHz", 41.40, 42.60},
};
}

const Band* bands(int& count) { count = (int)(sizeof kBands / sizeof kBands[0]); return kBands; }

std::string bandOf(double mhz) {
    for (const Band& b : kBands) if (mhz >= b.loMhz && mhz <= b.hiMhz) return b.name;
    return "";
}

std::vector<double> scanPlan(double sampleRate) {
    std::vector<double> out;
    const double w = 0.8 * sampleRate / 1e6;   // MHz one radio position takes in
    if (w <= 0.01) return out;
    for (const Band& b : kBands) {
        const double span = b.hiMhz - b.loMhz;
        const int n = std::max(1, (int)std::ceil(span / w - 1e-9));
        for (int i = 0; i < n; i++) out.push_back(std::round((b.loMhz + span * (i + 0.5) / n) * 1000) / 1000);
    }
    return out;
}

std::string hintFor(double centreMhz, double bwKhz, double periodSec) {
    char b[240], per[32] = "";
    const double res = bwKhz > 0 ? 150.0 / bwKhz : 0;   // range resolution c / 2B, km
    const char* cls = centreMhz <= 0 ? "" : centreMhz < 8 ? ", long range band" : centreMhz < 20 ? ", medium range band" : centreMhz < 30 ? ", short range band" : ", very short range band";
    if (periodSec > 0) snprintf(per, sizeof per, " every %.2f s", periodSec);
    if (periodSec > 0.35 && periodSec < 1.2 && bwKhz >= 15 && bwKhz <= 150)
        snprintf(b, sizeof b, "possible SeaSonde-like radar: %.0f kHz sweep%s (about %.1f km range resolution)%s", bwKhz, per, res, cls);
    else
        snprintf(b, sizeof b, "possible HF ocean radar: %.0f kHz sweep%s (about %.1f km range resolution)%s", bwKhz, per, res, cls);
    return b;
}

} // namespace codar
} // namespace dect2
