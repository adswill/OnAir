// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Scroll to map zoom levels (issue #652). A mouse wheel sends whole notches (1.0 each); an Apple trackpad or Magic Mouse sends dozens of small, smooth
// deltas per swipe and then a tail of momentum events. One zoom level per event made a light swipe jump to the minimum or maximum zoom. Here every
// event is clamped, the small ones add up until they make a notch, one gesture gives a level at most every kMinGapSec, and what is left over when
// the gesture pauses (or turns round) is dropped. A whole notch (a wheel) steps at once, as before. No UI code: a test can feed it events.
#pragma once
#include <cmath>

namespace dect2 {

class ScrollZoom {
public:
    static constexpr double kNotch = 1.0;       // the accumulated scroll that makes one level
    static constexpr double kMaxEvent = 1.0;    // one event counts for at most a notch (a burst of large smooth deltas)
    static constexpr double kMaxAcc = 1.5;      // the most that is kept waiting
    static constexpr double kMinGapSec = 0.20;  // smooth scrolling: at most one level per this time
    static constexpr double kIdleSec = 0.35;    // a pause this long ends a gesture: the rest is dropped

    // One scroll event (delta > 0 zooms in, < 0 out) at time nowSec; returns -1, 0 or +1 levels to zoom now.
    int feed(double delta, double nowSec) {
        if (!(std::fabs(delta) > 0) || std::isnan(delta)) return 0;
        if (nowSec - lastEvent_ > kIdleSec) acc_ = 0;                  // a new gesture
        lastEvent_ = nowSec;
        const double d = delta > kMaxEvent ? kMaxEvent : delta < -kMaxEvent ? -kMaxEvent : delta;
        if (std::fabs(delta) >= 0.99) {                                // a wheel notch: at once
            acc_ = 0; lastStep_ = nowSec;
            return delta > 0 ? 1 : -1;
        }
        if ((d > 0) != (acc_ > 0) && acc_ != 0) acc_ = 0;              // turned round
        acc_ += d;
        if (acc_ > kMaxAcc) acc_ = kMaxAcc;
        if (acc_ < -kMaxAcc) acc_ = -kMaxAcc;
        if (std::fabs(acc_) >= kNotch && nowSec - lastStep_ >= kMinGapSec) {
            const int s = acc_ > 0 ? 1 : -1;
            acc_ = 0;                                                  // the surplus (momentum) is dropped, not saved for the next level
            lastStep_ = nowSec;
            return s;
        }
        return 0;
    }
private:
    double acc_ = 0, lastEvent_ = -1e9, lastStep_ = -1e9;
};

} // namespace dect2
