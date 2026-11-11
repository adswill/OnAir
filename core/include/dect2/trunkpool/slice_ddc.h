// One channel out of a wide capture (single-radio trunking, docs/TRUNK1-3.9.md): mixed to 0 Hz, low-passed and decimated to a narrow rate
// (48 to 96 kHz) in one or two polyphase FIR stages (GroupDecimator), so a voice or control channel decoder runs at the narrow rate instead of
// mixing and filtering every sample of the capture itself. Cost per channel: one complex mix per input sample plus about 5.5 x input rate
// multiply-adds per component for the first stage (whatever its decimation), and a few multiply-adds per output sample for the second.
// Header only. The channel's offset is from the centre of the capture.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/dsp/group_decimator.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace dect2 {
namespace tpool {

class SliceDdc {
public:
    // rate: the capture's sample rate; offsetHz: the channel from the centre; halfBwHz: half the channel's width kept (signal plus the
    // decoder's own margin); minOutHz: the lowest output rate the decoders accept. False when the rate is not usable.
    bool configure(double rate, double offsetHz, double halfBwHz = 9000, double minOutHz = 48000) {
        on_ = false; two_ = false; mixOnly_ = false;
        in_ = rate; off_ = offsetHz;
        if (!(rate > 0) || !(halfBwHz > 0) || !(minOutHz > 2 * halfBwHz)) return false;
        const int dtot = (int)std::floor(rate / minOutHz);
        if (dtot < 2) {   // already narrow: only the mix
            mixOnly_ = true; out_ = rate;
            ph_ = std::complex<double>(1, 0);
            st_ = std::polar(1.0, -2 * 3.14159265358979323846 * offsetHz / rate);
            on_ = true;
            return true;
        }
        if (dtot <= 16) {
            if (!s1_.configure(rate, offsetHz, halfBwHz, dtot, 0.25 * minOutHz)) return false;
            out_ = s1_.outRate();
            on_ = true;
            return true;
        }
        // two stages: the first to about 8 x minOutHz (a wide transition, few taps per output), the second to the narrow rate
        const int d1 = std::max(2, dtot / 8);
        const double r1 = rate / d1;
        const int d2 = std::max(2, (int)std::floor(r1 / minOutHz));
        if (!s1_.configure(rate, offsetHz, halfBwHz, d1, 0.25 * minOutHz)) return false;
        if (!s2_.configure(r1, 0.0, halfBwHz, d2, 0.25 * minOutHz)) return false;
        two_ = true; out_ = s2_.outRate();
        on_ = true;
        return true;
    }
    bool on() const { return on_; }
    double inRate() const { return in_; }
    double outRate() const { return out_; }
    double offsetHz() const { return off_; }
    // n capture samples in; the channel at the narrow rate out (valid until the next call)
    const std::vector<cf32>& process(const cf32* x, size_t n) {
        if (!on_ || !x || !n) { buf_.clear(); return buf_; }
        if (mixOnly_) {
            buf_.resize(n);
            for (size_t i = 0; i < n; i++) {
                cf32 v = x[i];
                if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
                buf_[i] = cf32((float)(v.real() * ph_.real() - v.imag() * ph_.imag()), (float)(v.real() * ph_.imag() + v.imag() * ph_.real()));
                ph_ *= st_;
                if ((i & 1023) == 1023) ph_ /= std::abs(ph_);
            }
            return buf_;
        }
        const std::vector<cf32>& a = s1_.process(x, n);
        if (!two_) return a;
        return s2_.process(a.data(), a.size());
    }

private:
    bool on_ = false, two_ = false, mixOnly_ = false;
    double in_ = 0, out_ = 0, off_ = 0;
    GroupDecimator s1_, s2_;
    std::complex<double> ph_{1, 0}, st_{1, 0};
    std::vector<cf32> buf_;
};

} // namespace tpool
} // namespace dect2
