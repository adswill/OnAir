// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LBJ modem: 1200 bit/s 2-FSK (about +-4 kHz) on a 25 kHz railway channel -> POCSAG codewords. The signal is weak and the
// transmitters (locomotives) are up to a few kHz off frequency, so the chain is: mix, resample to 48 kHz, 6 kHz channel filter,
// FM discriminator, 1.5 kHz smoothing, a running mean as the slicer threshold, and a sync-word search at all 40 sample phases in
// both polarities. After a sync the bits are read at the found phase, batch by batch, re-timed on each following sync word.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/lbj/lbj_pocsag.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2::lbj {

class Modem {
public:
    static constexpr double kWork = 48000.0;
    static constexpr int kSps = 40;                  // samples per bit at 48 kHz

    void configure(double inputRate);
    void setOffset(double hz);                       // where the channel sits relative to 0 Hz in the input
    void reset();
    // Appends the bursts completed by these samples to `out`.
    void feed(const cf32* x, size_t n, std::vector<Burst>& out);

    double levelDb() const { return levelDb_; }
    double snrDb() const { return snrDb_; }
    double cfoHz() const { return cfoHz_; }
    double timeSec() const { return inCount_ / (inputRate_ > 0 ? inputRate_ : 1); }
    bool inBurst() const { return lockActive_ || pendActive_; }
    uint64_t syncs() const { return syncs_; }

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1r = 0, z2r = 0, z1i = 0, z2i = 0;
        void lowpass(double fc, double fs, double q);
        void clear() { z1r = z2r = z1i = z2i = 0; }
        inline double run(double x, double& z1, double& z2) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    };
    struct Hit { uint64_t n; int errs; bool inv; };

    void process(cf32 v, std::vector<Burst>& out);
    double soft(uint64_t idx) const { return ring_[idx & kMask]; }
    void finishPending();
    bool serviceLock(std::vector<Burst>& out);

    static constexpr size_t kRing = 1 << 16, kMask = kRing - 1;
    double inputRate_ = 0, offsetHz_ = 0, phase_ = 0;
    bool ready_ = false;
    ExactResampler rs_;
    std::vector<cf32> rsBuf_, mixBuf_;
    Biquad lp1_, lp2_, post_;
    cf32 prev_{0, 0};
    std::vector<double> ring_ = std::vector<double>(kRing, 0.0);
    uint64_t n_ = 0;                                  // 48 kHz sample counter
    double inCount_ = 0;
    double dc_ = 0, pw_ = 0, floor_ = 0;
    double levelDb_ = -200, snrDb_ = 0, cfoHz_ = 0;
    uint32_t reg_[kSps] = {}, prevWord_[kSps] = {};
    // pending sync (collecting neighbouring phases) and active lock
    bool pendActive_ = false; uint64_t pendUntil_ = 0; std::vector<Hit> hits_;
    bool lockActive_ = false; uint64_t end_ = 0; bool inv_ = false; double lockDc_ = 0, lockPw_ = 0; int batches_ = 0;
    Burst cur_;
    uint64_t syncs_ = 0;
};

} // namespace dect2::lbj
