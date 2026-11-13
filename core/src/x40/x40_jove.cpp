// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radio Jove (20.1 MHz, Jupiter and solar radio bursts): a power-versus-time burst finder. The band power is taken in 5 ms blocks; the baseline and its
// scatter follow the quiet level (blocks inside a burst are left out), and a burst is a run of blocks more than 4 scatter widths above it. Bursts of
// at most 100 ms are called short, longer ones long; the 5 ms resolution cannot resolve the millisecond structure of real S-bursts. Terrestrial
// interference (broadcast carriers, lightning crashes, power-line noise) also makes bursts: nothing here tells it from Jupiter or the Sun. Receive only.
#include "x40_int.h"

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 12000, kCut = 5000, kBlock = 0.005;
constexpr float kSigma = 4.f;
class JoveDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, kCut)) return false;
        rate_ = dec_.outRate(); blockN_ = std::max(8, (int)(rate_ * kBlock));
        reset();
        return true;
    }
    void reset() override { dec_.reset(); acc_ = 0; cnt_ = 0; t_ = 0; base_ = -1; var_ = 0; run_ = 0; in_ = false; blocks_ = 0; }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) {
            acc_ += std::norm(a); t_ += 1.0 / rate_;
            if (++cnt_ >= blockN_) { block(acc_ / (float)cnt_); acc_ = 0; cnt_ = 0; }
        });
    }
    void fill(X40Telemetry& t) const override {
        t.headline = base_ < 0 ? "waiting for data" : fmt("%d bursts, baseline %.1f dB", bursts_, toDb(base_));
        if (base_ >= 0) {
            t.facts.push_back({"baseline power", fmt("%.1f dB (relative)", toDb(base_))});
            t.facts.push_back({"baseline scatter", fmt("%.2f dB", scatterDb())});
            t.facts.push_back({"bursts", fmt("%d (%d short, %d long)", bursts_, shortB_, bursts_ - shortB_)});
            t.facts.push_back({"time resolution", fmt("%.0f ms", kBlock * 1e3)});
        }
    }
private:
    float scatterDb() const { return base_ > 0 ? 4.34f * std::sqrt(var_) / base_ : 0; }
    void block(float p) {
        blocks_++;
        if (base_ < 0) { base_ = p; var_ = 0; return; }
        const float sd = std::sqrt(var_);
        const bool warm = blocks_ > 400;                       // 2 s of baseline before any decision
        const bool hi = warm && p > base_ + kSigma * std::max(sd, 0.02f * base_);
        if (hi) {
            run_++; if (run_ >= 3) { in_ = true; out->energy(); }
            if (run_ == 1) { start_ = t_; peak_ = p; } else peak_ = std::max(peak_, p);
            if (in_ && t_ - start_ > 20.0) { base_ = p; var_ = 0; in_ = false; run_ = 0; blocks_ = 0; }   // a level step, not a burst: follow it
            return;
        }
        if (in_) {
            const double len = t_ - start_;
            bursts_++; const bool shortBurst = len <= 0.1; if (shortBurst) shortB_++;
            const float up = toDb(peak_ / base_);
            out->good(up, 0);
            out->row(shortBurst ? "Short burst" : "Long burst", fmt("%.2f s", len), fmt("%.1f dB above the baseline", up), toDb(peak_));
        }
        in_ = false; run_ = 0;
        const float a = 0.002f;                                  // baseline tau about 2.5 s
        const float d = p - base_;
        base_ += a * d;
        var_ += a * (d * d - var_);
    }
    Decimator dec_;
    double rate_ = 1, t_ = 0, start_ = 0; int blockN_ = 1, cnt_ = 0, run_ = 0, bursts_ = 0, shortB_ = 0; uint64_t blocks_ = 0;
    float acc_ = 0, base_ = -1, var_ = 0, peak_ = 0; bool in_ = false;
};
}

std::unique_ptr<Decoder> makeJove() { return std::make_unique<JoveDec>(); }

} // namespace dect2::x40
