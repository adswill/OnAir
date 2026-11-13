// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LEO carrier detector on generated spectrum frames (100 kHz span, 1000 bins, 100 ms frames, 20 s): a carrier drifting -200 Hz/s is
// found once with its start frequency, drift, first-seen time and length; a fixed carrier and a 2-frame blip are not reported;
// empty input gives nothing.
#include "dect2/specan/leo_detect.h"
#include <cmath>
#include <random>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const LeoBand band{137.000e6, 100.0};
    const size_t nBins = 1000, nFrames = 200;
    const double frameMs = 100.0;
    std::mt19937 rng(11);
    std::normal_distribution<float> noise(0.f, 0.5f);
    // Gaussian-shaped carrier with a width of about 0.6 bin, so the peak needs the parabolic refinement.
    auto addCarrier = [&](std::vector<float>& db, double hz, float level) {
        const double pos = (hz - band.startHz) / band.binHz;
        for (size_t k = 0; k < nBins; k++) {
            const double d = ((double)k - pos) / 0.6;
            db[k] += level * (float)std::exp(-0.5 * d * d);
        }
    };
    std::vector<LeoFrame> frames(nFrames);
    for (size_t i = 0; i < nFrames; i++) {
        LeoFrame& fr = frames[i];
        fr.tMs = (double)i * frameMs;
        fr.db.assign(nBins, 0.f);
        for (auto& x : fr.db) x += noise(rng);
        addCarrier(fr.db, 137.030e6 - 200.0 * fr.tMs / 1000.0, 20.f);   // the satellite: drifts down 200 Hz/s
        addCarrier(fr.db, 137.070e6, 20.f);                             // a fixed carrier
        if (i == 5 || i == 6) addCarrier(fr.db, 137.050e6, 20.f);       // a 2-frame blip
    }

    auto found = detectDopplerCarriers(frames, band);
    CHECK(found.size() == 1, "carriers %zu", found.size());
    if (found.size() == 1) {
        const LeoCarrier& c = found[0];
        CHECK(std::fabs(c.carrierHz - 137.030e6) < 100, "carrier %.1f Hz", c.carrierHz);
        CHECK(std::fabs(c.driftHzPerS + 200) < 5, "drift %.2f Hz/s", c.driftHzPerS);
        CHECK(c.firstSeenMs < 200, "first seen %.1f ms", c.firstSeenMs);
        CHECK(c.durationMs > 19000, "duration %.1f ms", c.durationMs);
        CHECK(c.hits >= 190, "hits %zu", c.hits);
    }
    CHECK(detectDopplerCarriers({}, band).empty(), "empty input gave carriers");
    return testSummary("specan LEO carrier detector");
}
