// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Finds a channel that is not in the middle of the sample band (a recording made with gqrx or SDR# at an offset, a radio tuned beside the
// channel) and moves it to the middle, before a receiver whose own search only covers a small range around the centre.
//
// The first `measureSec` seconds of input after configure() / reset() go into an averaged power spectrum. The receiver gets the samples as
// they are first: only when it has not locked after `waitSec` is the strongest block of width `bwHz` (6 dB above the floor, its middle from
// its half-power edges) mixed to the centre, and only when it lies beyond +-keepWithinHz, the range the receiver's own search covers. So a
// strong neighbour never pulls a receiver off a channel it can find by itself.
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <vector>

namespace dect2 {

class ChannelCentre {
public:
    void configure(double rate, double bwHz, double keepWithinHz, double waitSec, double measureSec = 0.3);
    void reset();                                  // measure again (after a retune)
    void process(cf32* x, size_t n, bool locked);  // in place: measures, then mixes by -offsetHz(); `locked`: the receiver has the signal
    bool decided() const { return decided_; }
    double offsetHz() const { return offset_; }    // where the channel was found (0: in the middle, or nothing found)
    // the offset was just found: the samples given so far were not mixed (a receiver may restart its search); cleared by the call
    bool takeChanged() { const bool c = changed_; changed_ = false; return c; }
    // Opt-in: instead of staying on the first channel found for good, a receiver that has not locked goes back to the middle after `dwellSec`, then
    // tries the next strongest block (up to three), then the middle again for twice as long, and so on until it locks (a strong neighbour on
    // the band next to a weak channel, #275). Without it the receiver is left on whatever it was moved to.
    void setRetry(double dwellSec) { retry_ = dwellSec > 0; dwell_ = dwellSec; }
    void freeze() { decided_ = true; }   // stop hunting: the decoder has locked (do not mix later and flush the buffer)

private:
    void measure();
    double rate_ = 0, bw_ = 0, keep_ = 0, wait_ = 1, measure_ = 0.3, cand_ = 0;
    bool haveCand_ = false, measured_ = false;
    int n_ = 0;
    std::vector<cf32> blk_;
    std::vector<double> acc_;
    size_t fill_ = 0, seen_ = 0;
    bool decided_ = true, changed_ = false;
    double offset_ = 0, ph_ = 0, dph_ = 0;
    bool retry_ = false;
    double dwell_ = 4;
    std::vector<double> cands_;          // the blocks to try, strongest first
    size_t tryIdx_ = 0;
    bool trying_ = false;
    size_t phaseSeen_ = 0, homeLen_ = 0;
};

} // namespace dect2
