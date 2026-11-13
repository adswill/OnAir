// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/qo100/qo100_gen.h"
#include "dect2/qo100/qo100_dsp.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace dect2 {

using namespace qo100;

double qo100SynthErrHz(const SynthConfig& c, double t) {
    const double drift = c.modeVal[1] != 0 ? c.modeVal[1] : kQo100SynthDriftHzPerMin;
    return kQo100SynthErrHz + c.modeVal[0] + drift * t / 60.0;
}

double qo100SynthCentreHz(const SynthConfig& c, double fs) {
    return c.modeVal[2] > 0 ? c.modeVal[2] * 1e6 : fs >= 9e6 ? kWholeCentreHz : kNarrowCentreHz;
}

namespace {

class Qo100Synth : public ModeSynth {
public:
    Qo100Synth(const SynthConfig& c, double fs) : c_(c), fs_(fs), fft_(kL) {
        rc_ = qo100SynthCentreHz(c, fs);
        inv_ = invertedLo(c.modeVal[3] > 0 ? c.modeVal[3] * 1e6 : 9750e6);
        quiet_ = c.modeOpt[0] == 1;
        sigma_ = 0.05 * std::sqrt(std::min(1.0, fs / 10e6));
        for (int i = 0; i < kG; i++) gtab_[i] = (float)gauss();
        // spectrum of the noise blocks: white, plus the DATV carriers (flat-topped, roll-off 0.25)
        gain_.assign(kL, 1.f);
        if (!quiet_) {
            const double e0 = qo100SynthErrHz(c, 0);
            struct D { double rf, sym, psd; } dv[2] = {{kDatvBeaconHz, kDatvBeaconSym, 10.0}, {kQo100Datv2Hz, kQo100Datv2Sym, 6.0}};
            for (int k = 0; k < kL; k++) {
                const int kk = k < kL / 2 ? k : k - kL;
                const double f = kk * fs / kL;
                double g2 = 1;
                for (const D& d : dv) {
                    const double u = std::fabs(f - (d.rf - e0 - rc_)), a = 0.25, f1 = d.sym * (1 - a) / 2, f2 = d.sym * (1 + a) / 2;
                    if (u <= f1) g2 += d.psd;
                    else if (u < f2) g2 += d.psd * 0.5 * (1 + std::cos(M_PI * (u - f1) / (a * d.sym)));
                }
                gain_[(size_t)k] = (float)std::sqrt(g2);
            }
            // the carriers: CW beacons, PSK beacon, test carrier, then 4 SSB stations of 3 tones
            add(kLowBeaconHz, 0.006f, 0); add(kHighBeaconHz, 0.006f, 0); add(kPskBeaconHz, 0.005f, 1); add(kQo100TestCarrierHz, 0.004f, 0);
            static const double kAud[3] = {450, 1100, 2100};
            for (int s = 0; s < 4; s++) for (int j = 0; j < 3; j++) add(kQo100SsbDialsHz[s] + kAud[j] + 37 * s, 0.0022f, 2 + s);
        }
        blk_.resize(kL);
    }
    double sampleRate() const override { return fs_; }
    void generate(cf32* out, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            if (pos_ >= kL) nextBlock();
            if (sub_ == 0) subBlock();
            float accR = blk_[(size_t)pos_].real(), accI = blk_[(size_t)pos_].imag();
            pos_++;
            const size_t nt = rr_.size();
            if (nt) {
                if (pskIdx_ >= 0) {   // the BPSK beacon: the sign turns over (linearly in the first quarter of a symbol)
                    const double ph = (double)n_ * 400.0 / fs_;
                    const uint64_t sym = (uint64_t)ph;
                    if (sym != pskSym_) { pskSym_ = sym; pskPrev_ = pskCur_; lfsr_ = lfsr_ * 6364136223846793005ull + 1442695040888963407ull; pskCur_ = (lfsr_ >> 63) ? 1.f : -1.f; }
                    const double u = ph - (double)sym;
                    am_[(size_t)pskIdx_] = pskAmp_ * (u < 0.25 ? pskPrev_ + (pskCur_ - pskPrev_) * (float)(u * 4) : pskCur_);
                }
                for (size_t k = 0; k < nt; k++) {
                    accR += am_[k] * rr_[k]; accI += am_[k] * ri_[k];
                    const float r = rr_[k] * sr_[k] - ri_[k] * si_[k];
                    ri_[k] = rr_[k] * si_[k] + ri_[k] * sr_[k];
                    rr_[k] = r;
                }
            }
            out[i] = inv_ ? cf32(accR, -accI) : cf32(accR, accI);
            n_++;
            if (++sub_ >= kSub) sub_ = 0;
        }
    }

private:
    static constexpr int kL = 65536, kG = 8192, kSub = 1024;
    void add(double rf, float amp, int kind) {
        if (kind == 1) { pskIdx_ = (int)rr_.size(); pskAmp_ = amp; }
        rf_.push_back(rf); base_.push_back(amp); kind_.push_back(kind);
        rr_.push_back(1.f); ri_.push_back(0.f); sr_.push_back(1.f); si_.push_back(0.f); am_.push_back(amp);
        const double ph = 2 * M_PI * (double)(rr_.size() * 0.618034);
        rr_.back() = (float)std::cos(ph); ri_.back() = (float)std::sin(ph);
    }
    // every kSub samples: the oscillator's error now, the steps of the carriers, the SSB envelopes, renormalised rotators
    void subBlock() {
        const double t = ((double)n_ + kSub / 2) / fs_, e = qo100SynthErrHz(c_, t);
        for (size_t k = 0; k < rr_.size(); k++) {
            const double w = 2 * M_PI * (rf_[k] - e - rc_) / fs_;
            sr_[k] = (float)std::cos(w); si_[k] = (float)std::sin(w);
            const float m = std::sqrt(rr_[k] * rr_[k] + ri_[k] * ri_[k]);
            if (m > 0) { rr_[k] /= m; ri_[k] /= m; }
            if (kind_[k] >= 2) {   // a syllable rhythm, each station its own
                const int s = kind_[k] - 2;
                const double v = std::sin(2 * M_PI * (2.3 + 0.4 * s) * t + 1.7 * s + 0.9 * (double)(k % 3));
                am_[k] = base_[k] * (float)(0.25 + 0.75 * std::max(0.0, v));
            }
        }
    }
    double uni() { s_ ^= s_ << 13; s_ ^= s_ >> 7; s_ ^= s_ << 17; return (double)(s_ >> 11) * (1.0 / 9007199254740992.0); }
    double gauss() {
        double u;
        do { u = uni(); } while (u <= 1e-300);
        return std::sqrt(-2 * std::log(u)) * std::cos(2 * M_PI * uni());
    }
    // a block of noise made in the frequency domain: Gaussian bins times the wanted shape, one inverse FFT
    void nextBlock() {
        const float sc = (float)(sigma_ / std::sqrt(2.0 * kL));
        for (int k = 0; k < kL; k++) {
            s_ ^= s_ << 13; s_ ^= s_ >> 7; s_ ^= s_ << 17;
            const float g = gain_[(size_t)k] * sc;
            blk_[(size_t)k] = cf32(g * gtab_[s_ & (kG - 1)], g * gtab_[(s_ >> 20) & (kG - 1)]);
        }
        fft_.inverse(blk_.data());
        pos_ = 0;
    }
    SynthConfig c_;
    double fs_, rc_ = 0, sigma_ = 0.05;
    bool inv_ = false, quiet_ = false;
    Fft fft_;
    std::vector<float> gain_;
    std::vector<cf32> blk_;
    float gtab_[kG];
    int pos_ = kL, sub_ = 0;
    uint64_t n_ = 0, s_ = 0x9E3779B97F4A7C15ull;
    std::vector<double> rf_;
    std::vector<float> base_, rr_, ri_, sr_, si_, am_;
    std::vector<int> kind_;
    int pskIdx_ = -1;
    float pskAmp_ = 0, pskPrev_ = 1, pskCur_ = 1;
    uint64_t pskSym_ = ~0ull, lfsr_ = 12345;
};

} // namespace

std::unique_ptr<ModeSynth> makeQo100Synth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<Qo100Synth>(cfg, sampleRate);
}

} // namespace dect2
