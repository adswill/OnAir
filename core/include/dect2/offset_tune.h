// Offset tuning: an alternative to removing the DC spike. The radio is tuned beside the channel, so its own DC spike (the LO leaking in)
// falls outside the channel, and the channel is shifted back to the centre digitally before any receiver sees it. The radio then needs a
// higher sample rate to hold the channel off its centre; planOffset() works out the offset and that rate, or says why it cannot be done.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <string>

namespace dect2 {

using cf32 = std::complex<float>;

struct OffsetPlan {
    bool ok = false;
    double offsetHz = 0;    // the radio is tuned this far BELOW the channel; the channel arrives at +offsetHz
    double rateHz = 0;      // the sample rate the radio must run at for that
    std::string why;        // when !ok: what stopped it, for the log
};

// channelHz: the width the receiver needs; modeRate: the rate the mode runs at without an offset; maxRate/minRate: the radio's limits
// (0 = unknown). The channel's lower edge sits a guard band above DC, and as much room is left above its upper edge.
inline OffsetPlan planOffset(double channelHz, double modeRate, double maxRate, double minRate = 0) {
    OffsetPlan p;
    if (!(channelHz > 0) || !(modeRate > 0)) { p.why = "no channel width known"; return p; }
    const double guard = std::max(25e3, 0.05 * channelHz);
    p.offsetHz = channelHz / 2 + guard;
    const double need = 2 * (p.offsetHz + channelHz / 2 + guard);           // the channel's upper edge plus the guard, both sides of DC
    p.rateHz = std::max(modeRate, std::ceil(need / 250e3) * 250e3);          // in steps of 250 kHz
    if (minRate > 0 && p.rateHz < minRate) p.rateHz = minRate;
    if (maxRate > 0 && p.rateHz > maxRate + 1) {
        char b[160];
        snprintf(b, sizeof b, "offset tuning needs %.2f Msps for this channel, the radio reaches %.2f", p.rateHz / 1e6, maxRate / 1e6);
        p.why = b;
        return p;
    }
    p.ok = true;
    return p;
}

// Shifts a stream by -shiftHz (moves a signal at +shiftHz to DC), phase continuous from one block to the next. The phasor is renormalised
// every block so that float rounding cannot make it grow or shrink over hours.
class OffsetMixer {
public:
    void set(double shiftHz, double rateHz) {
        if (shiftHz == shift_ && rateHz == rate_) return;
        shift_ = shiftHz; rate_ = rateHz;
        step_ = std::polar(1.0, rateHz > 0 ? -2 * M_PI * shiftHz / rateHz : 0.0);
        ph_ = 1;
    }
    bool active() const { return shift_ != 0 && rate_ > 0; }
    void mix(cf32* x, size_t n) {
        if (!active()) return;
        std::complex<double> ph = ph_;
        for (size_t i = 0; i < n; i++) {
            x[i] *= cf32((float)ph.real(), (float)ph.imag());
            ph *= step_;
        }
        ph_ = ph / std::abs(ph);
    }

private:
    double shift_ = 0, rate_ = 0;
    std::complex<double> step_{1, 0}, ph_{1, 0};
};

} // namespace dect2
