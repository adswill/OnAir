// Samples that never came out of a radio, found from the times and sizes of its deliveries alone (no help from the driver needed).
// At each delivery the radio should have handed over (time since the anchor) x (its rate) samples; the shortfall is the lag. A delivery
// that comes late only adds to the lag for a moment; a lost transfer raises it for good, so the lag is the samples lost so far plus a delay
// that is never negative. The smallest lag over the last second (E) is therefore the samples lost a second ago, plus a little jitter.
// The floor F is the largest E seen, sinking by 0.05 % of the rate (a radio clock that runs fast lowers the lag slowly; real ones are within
// 0.01 %); a slow clock raises it slowly. A rise of F by more than the threshold in one step is a loss: 3 ms, or more for a radio whose
// deliveries jitter more (four times the median, over the last ten seconds, of how far the smallest lag of a quarter lay above E), and at
// least half of one of the radio's transfers (a radio loses whole transfers; one that sends 100 ms blocks jitters by several ms).
// Smaller rises, a steady trickle of single lost transfers, are summed in a store that drains twice as fast as F sinks (so neither jitter
// nor a slow clock fills it) and are counted when it holds more than the threshold. A loss is counted about a second after it happened.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dect2 {

class DeliveryCheck {
public:
    static constexpr int64_t kStepNs = 250000000;    // E and F are updated every quarter second
    static constexpr int kSpan = 4;                   // E: the smallest lag of the last four quarters
    static constexpr double kMinStepSec = 0.003, kSink = 0.0005;

    // Start again: the first delivery at or after holdUntilNs becomes the anchor (a radio pauses or stutters around a start or a retune)
    void restart(int64_t holdUntilNs) { hold_ = holdUntilNs; anchored_ = false; }

    // A delivery: the total samples offered so far and when (steady clock, ns), and the size of the radio's transfers (samples; 0 = not
    // known). Returns the samples found missing (0 nearly always).
    double observe(uint64_t offered, int64_t ns, double rate, double block = 0) {
        if (rate <= 0) return 0;
        if (!anchored_) {
            if (ns < hold_) return 0;
            t0_ = ns; off0_ = offered; cur_ = 1e300; stepEnd_ = ns + kStepNs; steps_ = 0; store_ = 0; nHist_ = 0;
            anchored_ = true;
            return 0;
        }
        const double lag = (double)(ns - t0_) * 1e-9 * rate - (double)(offered - off0_);
        cur_ = std::min(cur_, lag);
        if (ns < stepEnd_) return 0;
        q_[steps_ % kSpan] = cur_;
        cur_ = 1e300; stepEnd_ = ns + kStepNs;
        if (++steps_ < kSpan) return 0;
        double e = q_[0];
        for (int i = 1; i < kSpan; i++) e = std::min(e, q_[i]);
        const double newest = q_[(steps_ - 1) % kSpan], sink = kSink * rate * (double)kStepNs * 1e-9;
        if (steps_ == kSpan) { f_ = e; return 0; }
        const double thr = std::max({kMinStepSec * rate, 0.5 * block, 4.0 * spread(newest - e)});
        const double sunk = f_ - sink, f = std::max(e, sunk);
        double lost = 0;
        if (steps_ > 3 * kSpan && f - sunk > thr) lost = f - sunk;
        else {
            store_ = std::max(0.0, store_ + (f - f_) - sink);
            if (steps_ > 3 * kSpan && store_ > thr) { lost = store_; store_ = 0; }
        }
        f_ = f;
        return lost;
    }

private:
    static constexpr int kHist = 40;   // ten seconds of quarters
    // the jitter: the median of how far the smallest lag of each recent quarter lay above E (a median, so the few quarters around a loss do not count)
    double spread(double x) {
        hist_[(size_t)(nHist_++ % kHist)] = x;
        const int n = std::min(nHist_, kHist);
        double v[kHist];
        std::copy(hist_, hist_ + n, v);
        std::nth_element(v, v + n / 2, v + n);
        return v[n / 2];
    }
    double hist_[kHist] = {};
    int nHist_ = 0;
    bool anchored_ = false;
    int steps_ = 0;
    int64_t hold_ = 0, t0_ = 0, stepEnd_ = 0;
    uint64_t off0_ = 0;
    double cur_ = 0, q_[kSpan] = {}, f_ = 0, store_ = 0;
};

} // namespace dect2
