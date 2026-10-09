// Finds a channel that is not in the middle of the sample band (a recording made with gqrx or SDR# at an offset, a radio tuned beside the
// channel) and moves it to the middle, before a receiver whose own search only covers a small range around the centre.
//
// The first `measureSec` seconds of input after configure() / reset() go into an averaged power spectrum. The receiver gets the samples as
// they are first: only when it has not locked after `waitSec` is the strongest block of width `bwHz` (6 dB above the floor, its middle from
// its half-power edges) mixed to the centre, and only when it lies beyond +-keepWithinHz, the range the receiver's own search covers. So a
// strong neighbour never pulls a receiver off a channel it can find by itself.
#pragma once
#include "ring.h"
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
};

} // namespace dect2
