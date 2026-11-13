// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared by the Yaesu Fusion tests: a transmission through the C4FM modulator and the receiver.
#pragma once
#include "dect2/fusion/fusion_gen.h"
#include "dect2/fusion/fusion_rx.h"
#include "../common/impair.h"
#include <vector>

namespace fusiontest {

constexpr double kRate = 250000, kOff = -50000;

inline std::vector<dect2::cf32> signal(const std::vector<dect2::fusion::GenParams>& txs, double cfoHz = 0, double snrDb = 0, bool swap = false, double skipSec = 0, float amp = 0.5f) {
    std::vector<dect2::cf32> all;
    for (const auto& p : txs) {
        std::vector<uint8_t> bits;
        dect2::fusion::genTransmissionBits(p, bits);
        std::vector<dect2::cf32> x = dect2::fusion::modulateC4fm(bits, kRate, swap ? -(kOff + cfoHz) : kOff + cfoHz, amp, 400);
        if (swap) for (auto& v : x) v = std::conj(v);   // a mirrored spectrum (I and Q swapped in the radio): the channel is still at kOff, upside down
        all.insert(all.end(), x.begin(), x.end());
    }
    if (snrDb > 0) impair::noise(all, snrDb, 5);
    if (skipSec > 0) impair::skip(all, (size_t)(skipSec * kRate));
    return all;
}

inline void run(dect2::FusionReceiver& r, const std::vector<dect2::cf32>& x) {
    r.configure(kRate);
    r.setSignalOffset(kOff);
    for (size_t i = 0; i < x.size(); i += 4096) r.feed(x.data() + i, std::min<size_t>(4096, x.size() - i));
}

inline dect2::FusionTelemetry last(dect2::FusionReceiver& r) {
    dect2::FusionTelemetry t;
    r.telemetry(t, 0);
    return t;
}

} // namespace fusiontest
