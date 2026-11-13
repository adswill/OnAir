// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sensitivity guard for the voice and pager modes: the mode's own test signal (makeModeSynth) at a weak SNR, rounded to 8 bits like a
// HackRF, through its receiver, in 50 ms chunks; the telemetry at the end. The tests compare what decoded with what decoded before the
// 0.3.0 receiver changes, so a filter or decoder change that costs sensitivity fails here instead of on the air.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace voicesens {

// `noiseOnly`: the receiver gets Gaussian noise instead of the signal (false decodes)
template <class Rx, class Tel>
Tel run(int stdMode, double rate, double tuneOffsetHz, double secs, double snrDb, double cfoHz = 0, int noiseSeed = 0, bool noiseOnly = false) {
    dect2::SynthConfig sc;
    sc.mode = stdMode; sc.snrDb = snrDb; sc.cfoHz = cfoHz;
    if (stdMode == 25) sc.modeOpt[3] = noiseSeed;   // the pager signal's noise seed (other modes use option 3 for other things)
    auto g = dect2::makeModeSynth(stdMode, sc, rate);
    Rx rx;
    if constexpr (requires { rx.setSilent(true); }) rx.setSilent(true);
    rx.configure(rate);
    if constexpr (requires { rx.setSignalOffset(1.0); }) rx.setSignalOffset(-tuneOffsetHz);
    std::vector<dect2::cf32> buf((size_t)(rate * 0.05));
    std::mt19937 rng(7);
    std::normal_distribution<double> nd(0, 1);
    for (double s = 0; s < secs; s += (double)buf.size() / rate) {
        if (g) g->generate(buf.data(), buf.size());
        if (noiseOnly) for (auto& v : buf) v = dect2::cf32((float)(nd(rng) * 0.02), (float)(nd(rng) * 0.02));
        for (auto& v : buf) v = dect2::cf32(std::round(v.real() * 127) / 127, std::round(v.imag() * 127) / 127);
        rx.feed(buf.data(), buf.size());
    }
    Tel t;
    rx.telemetry(t, 0);
    return t;
}

} // namespace voicesens
