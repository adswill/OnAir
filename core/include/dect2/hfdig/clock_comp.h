// Automatic correction of the computer clock for the slot-timed digital modes (FT8, FT4, FT2, WSPR ... and JS8), docs/EASE-3.8.md.
// Every station's DT in a slot is the measured time of its signal against the slot clock, so a wrong computer clock shows as the same DT on every
// station. ClockComp watches that median and asks for a correction once it is steady; if slots stay silent it asks once for a slot search, which finds slots
// up to half a period away. Measurement only: the caller moves its slot clock, the DT line keeps showing the correction, and TX slots are not touched.
#pragma once
#include <cmath>

namespace dect2 {

class ClockComp {
public:
    // Results of one finished slot of this receiver: decodes and the median DT of the decodes. Returns the seconds to subtract from the slot clock
    // now, 0 when none. wantWide tells once that a wider search should be tried (no decodes at all for kQuietSlots slots).
    double slotEnded(int decodes, double medianDt) {
        wantWide_ = false;
        if (decodes < 1) {
            consecutive_ = 0;
            if (quiet_ < 1000) quiet_++;
            if (quiet_ == kQuietSlots && !wideTried_) { wideTried_ = true; wantWide_ = true; }
            return 0;
        }
        quiet_ = 0;
        if (decodes < kMinDecodes && std::fabs(medianDt) < kBigDt) { consecutive_ = 0; return 0; }   // one decode can be a station with a bad clock: only a large error counts on it
        if (std::fabs(medianDt) < kApplyDt) { consecutive_ = 0; return 0; }
        const bool sameWay = consecutive_ > 0 && (medianDt > 0) == (last_ > 0);
        consecutive_ = sameWay ? consecutive_ + 1 : 1;
        last_ = medianDt;
        if (consecutive_ < kSteadySlots) return 0;
        consecutive_ = 0;
        total_ -= medianDt;
        return medianDt;
    }
    // A slot search found a shift (the same sign as the DT it removes)
    void searchFound(double shift) { total_ -= shift; wideTried_ = true; quiet_ = 0; }
    bool wantWide() const { return wantWide_; }
    double total() const { return total_; }                  // what has been applied so far, added to the computer clock (0 = the clock is right)
    bool wideTried() const { return wideTried_; }
    static constexpr double kApplyDt = 0.5, kBigDt = 1.0;
    static constexpr int kMinDecodes = 2, kSteadySlots = 2, kQuietSlots = 4;
private:
    double total_ = 0, last_ = 0;
    int consecutive_ = 0, quiet_ = 0;
    bool wideTried_ = false, wantWide_ = false;
};

}  // namespace dect2
