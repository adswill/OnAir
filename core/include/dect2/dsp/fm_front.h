// Small FM front end for the analog trunking control-channel modes (LTR, EDACS): mix the channel to 0 Hz, resample to a low working
// rate, channel filter, FM discriminator. The output is the instantaneous frequency in Hz. Header only.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/dsp/fftutil.h"
#include <memory>
#include <algorithm>
#include <cmath>
#include <vector>

namespace dect2 {

struct FmBiquad {   // second-order low pass (RBJ cookbook), double precision, real signals
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void lowpass(double fc, double fs, double q) {
        const double w = 2 * 3.14159265358979323846 * fc / fs, al = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + al;
        b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
        z1 = z2 = 0;
    }
    void clear() { z1 = z2 = 0; }
    double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

// Running high and low of a signal (fast attack, slow decay): the slicer threshold is the middle, the range tells whether there is a signal.
struct EyeEnvelope {
    double hi = 0, lo = 0, attack = 0.05, decay = 0.0005;
    void clear() { hi = lo = 0; }
    void push(double s) {
        hi += (s > hi ? attack : decay) * (s - hi);
        lo += (s < lo ? attack : decay) * (s - lo);
    }
    double mid() const { return 0.5 * (hi + lo); }
    double range() const { return hi - lo; }
};

class FmFront {
public:
    // workRate: sample rate of the output; chanHz: half-width of the channel filter (two cascaded sections)
    bool configure(double inRate, double workRate, double chanHz) {
        in_ = inRate; work_ = workRate;
        ok_ = inRate > 0 && workRate > 0 && inRate <= workRate * 4096 && rs_.configure(inRate, workRate, 8);
        for (int i = 0; i < 2; i++) { lr_[i].lowpass(chanHz, workRate, i ? 1.3066 : 0.5412); li_[i].lowpass(chanHz, workRate, i ? 1.3066 : 0.5412); }
        reset();
        return ok_;
    }
    bool ready() const { return ok_; }
    // Carrier follow-up (opt-in, 4.0): lets the channel filter be narrow without losing a signal that is off its nominal frequency
    // (a radio's reference error, 20 ppm is 17 kHz at 850 MHz). The mean power spectrum of the resampled channel is averaged over about
    // 0.4 s, the noise floor is taken from its quietest quarter, and the centroid of what stands above the floor inside +-searchHz moves
    // the mixer (never by more than maxHz in all). The discriminator mean would be pulled to the filter centre by the noise; the
    // centroid of the power spectrum is not. Does nothing until the channel carries at least a quarter of the noise power in the window.
    void setAfc(bool on, double searchHz = 9000.0, double maxHz = 20000.0) { afc_ = on; afcSearch_ = searchHz; afcMax_ = maxHz; adj_ = 0; afcFill_ = 0; afcBlocks_ = 0; }
    double afcOffsetHz() const { return adj_; }
    void setOffset(double hz) { if (hz != off_) adj_ = 0; off_ = hz; }   // where the channel sits relative to 0 Hz in the input
    void reset() {
        afcFill_ = 0; afcBlocks_ = 0; rs_.reset(); phRe_ = 1; phIm_ = 0; prev_ = cf32(0, 0); pw_ = 0;
        for (int i = 0; i < 2; i++) { lr_[i].clear(); li_[i].clear(); }
    }
    double levelDb() const { return pw_ > 0 ? 10 * std::log10(pw_ + 1e-30) : -200; }
    double workRate() const { return work_; }
    // Appends the instantaneous frequency (Hz) of the samples to out.
    void feed(const cf32* x, size_t n, std::vector<float>& out) {
        if (!ok_ || !x || !n) return;
        // the mixer: a phasor turned by one step per sample (double, renormalised every 1024 samples) instead of a cos and a sin per sample
        const double step = -2 * 3.14159265358979323846 * (off_ + adj_) / in_;
        if (step != stepW_) { stepW_ = step; stRe_ = std::cos(step); stIm_ = std::sin(step); }
        mix_.resize(n);
        double pr = phRe_, pi = phIm_;
        for (size_t i = 0; i < n; i++) {
            const float lr = (float)pr, li = (float)pi, xr = x[i].real(), xi = x[i].imag();
            mix_[i] = cf32(xr * lr - xi * li, xr * li + xi * lr);
            const double nr = pr * stRe_ - pi * stIm_;
            pi = pr * stIm_ + pi * stRe_; pr = nr;
            if ((i & 1023) == 1023) { const double m = 1.0 / std::sqrt(pr * pr + pi * pi); pr *= m; pi *= m; }
        }
        { const double m = 1.0 / std::sqrt(pr * pr + pi * pi); phRe_ = pr * m; phIm_ = pi * m; }
        rsb_.clear();
        rs_.process(mix_.data(), n, rsb_);
        const double k = work_ / (2 * 3.14159265358979323846);
        if (afc_) afcFeed();
        for (const cf32& v : rsb_) {
            double re = lr_[0].run(v.real()), im = li_[0].run(v.imag());
            re = lr_[1].run(re); im = li_[1].run(im);
            const cf32 y((float)re, (float)im);
            out.push_back((float)(std::arg(y * std::conj(prev_)) * k));
            prev_ = y;
            pw_ += (re * re + im * im - pw_) * 0.0005;
        }
    }
private:
    void afcFeed() {
        if (!afcFft_) {
            afcN_ = 128;
            while (afcN_ < work_ / 250.0 && afcN_ < 1024) afcN_ *= 2;
            afcFft_ = std::make_unique<Fft>(afcN_);
            afcWin_.resize((size_t)afcN_); afcBuf_.assign((size_t)afcN_, cf32(0, 0)); afcAvg_.assign((size_t)afcN_, 0.0);
            for (int i = 0; i < afcN_; i++) afcWin_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * (i + 0.5) / afcN_));
            afcFill_ = 0;
        }
        const double alpha = std::min(0.5, (double)afcN_ / (afcSec_ * work_));
        for (const cf32& v : rsb_) {
            afcBuf_[(size_t)afcFill_] = v * afcWin_[(size_t)afcFill_];
            if (++afcFill_ < afcN_) continue;
            afcFill_ = 0;
            afcFft_->forward(afcBuf_.data());
            for (int k = 0; k < afcN_; k++) {
                const double p = (double)std::norm(afcBuf_[(size_t)k]);
                afcAvg_[(size_t)k] += (afcBlocks_ == 0 ? 1.0 : alpha) * (p - afcAvg_[(size_t)k]);
            }
            if (++afcBlocks_ < (int)std::ceil(afcSec_ * work_ / afcN_)) continue;
            // floor: the quietest quarter of the bins
            afcTmp_ = afcAvg_;
            std::nth_element(afcTmp_.begin(), afcTmp_.begin() + afcN_ / 4, afcTmp_.end());
            const double floor = std::max(1e-30, afcTmp_[(size_t)(afcN_ / 4)]);
            double sw = 0, swf = 0, excess = 0; int nb = 0;
            for (int k = 0; k < afcN_; k++) {
                const int kk = k < afcN_ / 2 ? k : k - afcN_;
                const double f = kk * work_ / afcN_;
                if (std::fabs(f) > afcSearch_) continue;
                nb++;
                const double e = afcAvg_[(size_t)k] - floor;
                excess += e;
                if (afcAvg_[(size_t)k] < 1.4 * floor) continue;
                sw += e; swf += e * f;
            }
            if (sw > 0 && excess > 0.25 * floor * nb) {
                const double c = swf / sw;
                if (std::fabs(c) > afcDead_) {
                    adj_ = std::max(-afcMax_, std::min(afcMax_, adj_ + 0.8 * c));
                    afcBlocks_ = 0;     // the spectrum moved under the averages: start them again
                    continue;
                }
            }
            afcBlocks_ = (int)std::ceil(afcSec_ * work_ / afcN_) - 1;   // keep averaging, look again at the next block
        }
    }
    double afcDead_ = 500;
    double afcSec_ = 0.8;
    bool afc_ = false;
    double afcSearch_ = 9000, afcMax_ = 20000, adj_ = 0;
    int afcN_ = 0, afcFill_ = 0, afcBlocks_ = 0;
    std::unique_ptr<Fft> afcFft_;
    std::vector<float> afcWin_;
    std::vector<cf32> afcBuf_;
    std::vector<double> afcAvg_, afcTmp_;
    double in_ = 0, work_ = 0, off_ = 0, pw_ = 0;
    double phRe_ = 1, phIm_ = 0, stepW_ = 0, stRe_ = 1, stIm_ = 0;   // mixer phasor and its step
    bool ok_ = false;
    ExactResampler rs_;
    FmBiquad lr_[2], li_[2];
    cf32 prev_{0, 0};
    std::vector<cf32> mix_, rsb_;
};

} // namespace dect2
