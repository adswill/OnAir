// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Noise figure (Y-factor) meter: switch a noise source (or a termination against an antenna on a quiet band) on and off in front of the receiver and read the step. The power in the middle 200 kHz of the
// capture is taken every 25 ms; in 0.25 s windows the mean and spread are found; a plateau is a run of windows that agree within 0.15 dB; two plateaus that differ by 0.3 dB or more are a hot and a cold
// state (the higher is hot). Y = P_hot / P_cold, in dB and linear; the noise figure of the receiver is NF = ENR - 10 log(Y - 1) where ENR is the excess noise ratio of the source; since the program does not know it,
// NF is listed for ENR = 5, 6.5, 15 and 25 dB (read the line for your source). The average of the last four steps is shown with its spread. Needs a receiver that is linear in the band (no AGC, gain fixed) and
// noise (not a carrier) as the only thing in the 200 kHz; the cold state must not be below the receiver's own noise by much more than the source's ENR allows. The meter only knows the step: it cannot know that the
// source is a calibrated one. Receive only.
#include "x40_dsp.h"

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 250000, kCut = 100000, kBlockSec = 0.025, kWinSec = 0.25, kAgreeDb = 0.15, kStepDb = 0.3;
constexpr double kEnr[4] = {5.0, 6.5, 15.0, 25.0};

struct Plat { double db = 0, sd = 0, t0 = 0, t1 = 0; int n = 0; };

class YFactorDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, kCut)) return false;
        rate_ = dec_.outRate(); blockN_ = std::max<size_t>(16, (size_t)(kBlockSec * rate_)); winBlocks_ = std::max(2, (int)std::lround(kWinSec / kBlockSec));
        reset(); return true;
    }
    void reset() override { dec_.reset(); acc_ = 0; nAcc_ = 0; blk_.clear(); plats_.clear(); steps_.clear(); t_ = 0; skip_ = (size_t)(0.1 * rate_); have_ = false; lastY_ = 0; nSteps_ = 0; cur_ = Plat(); haveCur_ = false; }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) { if (skip_) { skip_--; return; } acc_ += std::norm(a); if (++nAcc_ >= blockN_) { block(10 * std::log10(std::max(acc_ / (double)nAcc_, 1e-20))); acc_ = 0; nAcc_ = 0; } });
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        t.facts.push_back({"power now", fmt("%.2f dB (relative, the middle 200 kHz)", haveCur_ ? cur_.db : 0.0)});
        if (steps_.empty()) { t.headline = "no hot / cold step yet: switch the noise source on and off (a step of 0.3 dB or more)"; return; }
        double m = 0; for (double v : steps_) m += v; m /= steps_.size(); double sd = 0; for (double v : steps_) sd += (v - m) * (v - m); sd = steps_.size() > 1 ? std::sqrt(sd / (steps_.size() - 1)) : 0;
        const double y = std::pow(10.0, m / 10);
        t.headline = fmt("Y = %.2f dB (%.3f), %d steps seen; NF for ENR 15 dB: %s", m, y, nSteps_, nfStr(15.0, y).c_str());
        t.facts.push_back({"Y-factor", fmt("%.3f dB (linear %.4f), spread %.3f dB over the last %zu steps", m, y, sd, steps_.size())});
        for (int i = 0; i < 4; i++) t.facts.push_back({fmt("noise figure if the source is ENR %.1f dB", kEnr[i]), nfStr(kEnr[i], y)});
        t.facts.push_back({"steps", fmt("%d seen", nSteps_)});
    }
private:
    static std::string nfStr(double enr, double y) { const double v = y - 1; if (v <= 0) return "-"; const double nf = enr - 10 * std::log10(v); return nf < 0 ? fmt("%.2f dB (negative: the source's ENR is lower than this step implies)", nf) : fmt("%.2f dB", nf); }
    void block(double db) {
        t_ += kBlockSec; blk_.push_back(db);
        if ((int)blk_.size() < winBlocks_) return;
        double m = 0; for (double v : blk_) m += v; m /= blk_.size(); double sd = 0; for (double v : blk_) sd += (v - m) * (v - m); sd = std::sqrt(sd / (blk_.size() - 1)); blk_.clear();
        have_ = true;
        if (!haveCur_) { cur_ = {m, sd, t_ - kWinSec, t_, 1}; haveCur_ = true; return; }
        if (std::fabs(m - cur_.db) <= kAgreeDb) { cur_.db = (cur_.db * cur_.n + m) / (cur_.n + 1); cur_.n++; cur_.t1 = t_; return; }
        // a different level: the plateau that ended counts if it lasted at least 2 windows
        if (cur_.n >= 2 && (plats_.empty() || t_ - plats_.back().t1 < 1e9)) {
            if (!plats_.empty() && std::fabs(cur_.db - plats_.back().db) >= kStepDb) {
                const double y = std::fabs(cur_.db - plats_.back().db); steps_.push_back(y); if (steps_.size() > 4) steps_.erase(steps_.begin()); nSteps_++;
                out->good((float)y, 0);
                out->row("Step", fmt("%.2f dB", y), fmt("%s: from %.2f to %.2f dB (relative power), Y = %.3f", cur_.db > plats_.back().db ? "source on" : "source off", plats_.back().db, cur_.db, std::pow(10.0, y / 10)), (float)cur_.db);
            }
            if (plats_.empty() || std::fabs(cur_.db - plats_.back().db) >= kAgreeDb) plats_.push_back(cur_); if (plats_.size() > 8) plats_.erase(plats_.begin());
        }
        cur_ = {m, sd, t_ - kWinSec, t_, 1};
    }
    Decimator dec_; double rate_ = 1, acc_ = 0, t_ = 0, lastY_ = 0; size_t blockN_ = 16, nAcc_ = 0, skip_ = 0; int winBlocks_ = 10, nSteps_ = 0; bool have_ = false, haveCur_ = false;
    std::vector<double> blk_, steps_; std::vector<Plat> plats_; Plat cur_;
};
}

std::unique_ptr<Decoder> makeYFactor() { return std::make_unique<YFactorDec>(); }

} // namespace dect2::x40
