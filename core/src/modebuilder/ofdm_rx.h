// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Internal: the receiver of the Mode Builder's OFDM block (Receiver uses it when the definition's modulation is OFDM).
#pragma once
#include "dect2/modebuilder/modebuilder.h"
#include <mutex>

namespace dect2::modebuilder {

struct Receiver::OfdmImpl {
    Definition d;
    double fsIn = 0, fsO = 0, extraOff = 0;
    std::vector<cf> buf;           // baseband at fsO, from `base` (sample index of buf[0])
    long long base = 0, scan = 0;
    double phase = 0, inPos = 0;   // mixer phase, input position for the resampler (fractional)
    cf prevIn{0, 0}, recentSum{0, 0};
    std::vector<cf> recent;        // the last samples of the anti-alias average
    long long inCount = 0;
    mutable std::mutex mu;
    std::vector<Frame> out;
    RxStats st;

    explicit OfdmImpl(const Definition& def) : d(def) {}
    void configure(double rate, double extra);
    void feed(const cf* x, size_t n);
    void process(bool final);
    void flush();
    std::vector<Frame> take();
    RxStats stats() const;
};

}   // namespace dect2::modebuilder
