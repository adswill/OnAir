// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// COSPAS-SARSAT second-generation beacon (SGB, C/S T.018) burst receiver. Receive only.
// Input: complex samples at exactly 153.6 kHz (4 samples per chip), nominal channel centre at 0 Hz. The 166.7 ms preamble is known (the first 6400 chips of
// the two PRN segments, all data bits zero), so the burst is found by a differential matched filter (insensitive to the carrier offset, +-38 kHz) done with
// FFTs over a ring of 1.7 s. Then: carrier refinement on the preamble in three steps, a second-order carrier loop (decision directed) and a chip timing
// loop over the 150 channel bits, BCH(250,202) repair and the message layer of sgb_proto.h.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/sarsat/sgb_proto.h"
#include <memory>
#include <vector>

namespace dect2::sarsat {

constexpr double kSgbFs = 153600.0;

struct SgbHit {
    Beacon b;
    double cfoHz = 0;
    float snrDb = 0;        // of the despread channel bits (symbol energy over noise), not a C/N0
    double endSample = 0;   // absolute index of the sample after the burst
};

// x holds a burst, whose start lies near sample `start` (+- a few samples), at kSgbFs. Fills `out` when the BCH check passes.
bool sgbDemodBurst(const std::vector<cf32>& x, long start, double cfoHz, SgbHit& out);

class SgbDetector {
public:
    SgbDetector();
    ~SgbDetector();
    void reset();
    void push(const cf32* x, size_t n);      // samples at kSgbFs
    std::vector<SgbHit> take();              // the bursts decoded since the last call
    uint64_t candidates() const;             // correlation peaks that looked like a preamble
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace dect2::sarsat
