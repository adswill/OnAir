// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NDB census: every beacon inside the captured band at once. Carriers are found in an averaged spectrum and each gets its own channel with the NDB decoder.
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <memory>
#include <vector>

namespace dect2 {

struct NdbCensusRow;

class NdbCensus {
public:
    NdbCensus();
    ~NdbCensus();
    // rate: input samples per second. dialAt: the baseband frequency of the dial (rows report the offset from it)
    void configure(double rate, double dialAt);
    void feed(const cf32* x, size_t n);
    void snapshot(std::vector<NdbCensusRow>& out) const;
    void clear();
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
