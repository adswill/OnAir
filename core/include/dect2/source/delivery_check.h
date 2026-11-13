// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

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

// The rate a radio actually delivers, as a percent of the rate it is set to, from the time stamps of the radio's own writes and not from the
// clock of whoever looks at the count. A radio hands over whole transfers, so a window on the observer's clock holds a varying number of them
// and reads up to one transfer short or long; at a low sample rate a transfer is a large part of the window (131072 samples are 0.5 s at
// 250 ksps) and the 3.9 window read 78 % of a radio that lost nothing: the "use a USB 3 port" warning at low rates. Here each window keeps
// its least delayed write (the smallest lag, as DeliveryCheck), and the rate is the samples between the least delayed writes of two windows
// over the time between them, so neither the transfer size nor a late transfer or a short OS hiccup moves it. A radio that stops writing
// reads low: once its last write is older than one transfer and a quarter of a second, the observer's clock stands in for the next write.
class DeliveryRate {
public:
    void restart() { origin_ = false; prev_ = cur_ = false; pct_ = -1.f; }
    float pct() const { return pct_; }
    // offered / ns / block: the ring's last write (total samples offered, its steady-clock time, its size); now: the observer's clock.
    // Returns the percent of the set rate over the last window (two seconds, or four transfers when that is longer), -1 until there is one.
    float observe(uint64_t offered, int64_t ns, size_t block, int64_t now, double rate) {
        if (rate <= 0) return pct_;
        if (!origin_ || offered < lastOff_) { restart(); origin_ = true; off0_ = offered; ns0_ = ns; startNs_ = now; }
        lastOff_ = offered;
        const double blockSec = (double)block / rate;
        const double win = std::max(2.0, 4.0 * blockSec);
        take(offered, ns, rate);
        const int64_t stale = (int64_t)((blockSec + 0.25) * 1e9);
        if (now - ns > stale) take(offered, now - stale, rate);   // nothing new for too long: the delivery is behind by at least that
        if ((double)(now - startNs_) * 1e-9 < win) return pct_;
        if (prev_ && cur_ && curNs_ - prevNs_ > 500000000LL) pct_ = (float)(100.0 * (double)(curOff_ - prevOff_) / (rate * (double)(curNs_ - prevNs_) * 1e-9));
        if (cur_) { prev_ = true; prevOff_ = curOff_; prevNs_ = curNs_; }
        cur_ = false;
        startNs_ = now;
        return pct_;
    }

private:
    void take(uint64_t off, int64_t ns, double rate) {
        const double lag = (double)(ns - ns0_) * 1e-9 - (double)(off - off0_) / rate;   // seconds behind the set rate (plus a constant)
        if (!cur_ || lag < curLag_) { cur_ = true; curLag_ = lag; curOff_ = off; curNs_ = ns; }
    }
    bool origin_ = false, prev_ = false, cur_ = false;
    uint64_t off0_ = 0, lastOff_ = 0, prevOff_ = 0, curOff_ = 0;
    int64_t ns0_ = 0, startNs_ = 0, prevNs_ = 0, curNs_ = 0;
    double curLag_ = 0;
    float pct_ = -1.f;
};

} // namespace dect2
