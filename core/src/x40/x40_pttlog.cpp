// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// PTT and duty-cycle logger for one channel (12.5 kHz): when the channel is keyed, for how long, and how busy it is. The power of the channel is taken in 5 ms blocks; the noise
// floor is the 10th percentile of the last minute (so the channel should be idle at least a tenth of the time), a transmission starts when the power is 6 dB over the floor and ends
// when it has stayed under 3 dB over the floor for 0.3 s (fading and short gaps inside one over do not split it; a longer gap does). Every finished transmission is one row:
// start, length, strength and the gap since the one before. Under 0.5 s counts as a "kerchunk" (a key-up with no speech). A transmission longer than 3 minutes is flagged once (a stuck
// key). Counts, duty cycle (whole run and last 10 minutes), mean, median and longest length and the rate per hour are kept. It sees what reaches this radio: a repeater that is
// heard but whose input is not is half the picture. Receive only.
#include "x40_dsp.h"
#include <deque>

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 16000, kCut = 6000, kBlockSec = 0.005, kOnDb = 6.0, kOffDb = 3.0, kHangSec = 0.3, kKerchunkSec = 0.5, kStuckSec = 180, kFloorWindowSec = 60, kWarmSec = 5;

class PttLogDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, kCut)) return false;
        rate_ = dec_.outRate(); blockN_ = std::max<size_t>(8, (size_t)(kBlockSec * rate_)); reset(); return true;
    }
    void reset() override {
        dec_.reset(); acc_ = 0; nAcc_ = 0; t_ = 0.1; hist_.clear();   // t_ includes the 0.1 s the filter start-up swallows
        floorDb_ = -120; haveFloor_ = false; on_ = false; belowSince_ = -1; start_ = 0; sumDb_ = 0; nDb_ = 0; stuckFlagged_ = false;
        lens_.clear(); nTx_ = 0; nKer_ = 0; busySec_ = 0; recent_.clear(); lastEnd_ = -1; longest_ = 0; blocks_ = 0; have_ = false; sortedDirty_ = true; skip_ = (size_t)(0.1 * rate_);
    }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) {
            if (skip_) { skip_--; return; }
            acc_ += std::norm(a); if (++nAcc_ < blockN_) return;
            block(10 * std::log10(std::max(acc_ / (double)nAcc_, 1e-18))); acc_ = 0; nAcc_ = 0;
        });
    }
    void fill(X40Telemetry& t) const override {
        if (!have_ || t_ < kWarmSec) { t.headline = "measuring the noise floor"; return; }
        const double el = t_ - kWarmSec;
        const double busyNow = busySec_ + (on_ ? t_ - start_ : 0.0);
        t.headline = on_ ? fmt("keyed for %.1f s, %d transmissions, duty %.1f %%", t_ - start_, nTx_, 100 * busyNow / std::max(el, 1e-9)) : fmt("idle, %d transmissions, duty %.1f %%", nTx_, 100 * busyNow / std::max(el, 1e-9));
        t.facts.push_back({"channel", on_ ? fmt("keyed for %.1f s", t_ - start_) : std::string("idle")});
        t.facts.push_back({"noise floor", fmt("%.1f dB (relative); keyed above +%.0f dB", floorDb_, kOnDb)});
        t.facts.push_back({"transmissions", fmt("%d (%d under %.1f s)", nTx_, nKer_, kKerchunkSec)});
        t.facts.push_back({"duty cycle", fmt("%.1f %% of %.0f s", 100 * busyNow / std::max(el, 1e-9), el)});
        double rb = 0; const double w = std::min(600.0, el);
        for (auto& r : recent_) { const double a = std::max(r.first, t_ - w), b = std::min(r.second, t_); if (b > a) rb += b - a; }
        if (on_) rb += t_ - std::max(start_, t_ - w);
        t.facts.push_back({"duty cycle, last 10 min", fmt("%.1f %% of %.0f s", 100 * rb / std::max(w, 1e-9), w)});
        if (!lens_.empty()) {
            std::vector<double> s(lens_); std::sort(s.begin(), s.end());
            double sum = 0; for (double v : s) sum += v;
            t.facts.push_back({"length", fmt("mean %.2f s, median %.2f s, longest %.2f s", sum / s.size(), s[s.size() / 2], longest_)});
            t.facts.push_back({"rate", fmt("%.1f transmissions per hour", nTx_ * 3600.0 / std::max(el, 1.0))});
        }
    }
private:
    void block(double db) {
        t_ += (double)blockN_ / rate_; blocks_++; have_ = true;
        hist_.push_back((float)db); const size_t cap = (size_t)(kFloorWindowSec / kBlockSec);
        if (hist_.size() > cap) hist_.pop_front();
        if (blocks_ % 200 == 0 || !haveFloor_) {            // once a second
            if (t_ >= 1.0) {
                std::vector<float> v(hist_.begin(), hist_.end()); const size_t k = v.size() / 10; std::nth_element(v.begin(), v.begin() + k, v.end());
                const double q = v[k];
                if (!on_ || !haveFloor_) { floorDb_ = haveFloor_ ? 0.9 * floorDb_ + 0.1 * q : q; haveFloor_ = true; }   // frozen while keyed
            }
        }
        if (!haveFloor_ || t_ < kWarmSec) return;
        const double over = db - floorDb_;
        if (!on_) {
            if (over > kOnDb) { on_ = true; start_ = t_ - kBlockSec; belowSince_ = -1; sumDb_ = 0; nDb_ = 0; stuckFlagged_ = false; out->good(0, 0); }
        } else {
            sumDb_ += over; nDb_++;
            if (over < kOffDb) { if (belowSince_ < 0) belowSince_ = t_; if (t_ - belowSince_ >= kHangSec) endTx(belowSince_); }
            else belowSince_ = -1;
            if (on_ && !stuckFlagged_ && t_ - start_ > kStuckSec) { stuckFlagged_ = true; out->row("Stuck key", fmt("%.0f s", t_ - start_), fmt("keyed since %.1f s, still keyed after %.0f s", start_, kStuckSec), (float)db); }
            if (on_) out->good(0, 0);
        }
        if (!on_) out->energy();
    }
    void endTx(double endT) {
        on_ = false;
        const double len = endT - start_;
        nTx_++; lens_.push_back(len); longest_ = std::max(longest_, len); busySec_ += len; recent_.push_back({start_, endT});
        while (!recent_.empty() && recent_.front().second < t_ - 600) recent_.pop_front();
        if (len < kKerchunkSec) nKer_++;
        const double gap = lastEnd_ >= 0 ? start_ - lastEnd_ : -1;
        lastEnd_ = endT;
        out->row(len < kKerchunkSec ? "Kerchunk" : "Transmission", fmt("#%d", nTx_), fmt("start %.1f s, length %.2f s, %.1f dB over the floor%s", start_, len, nDb_ ? sumDb_ / nDb_ : 0.0, gap >= 0 ? fmt(", gap %.1f s", gap).c_str() : ""), (float)(floorDb_ + (nDb_ ? sumDb_ / nDb_ : 0.0)));
    }
    Decimator dec_; double rate_ = 1, acc_ = 0, t_ = 0, floorDb_ = -120, belowSince_ = -1, start_ = 0, sumDb_ = 0, busySec_ = 0, lastEnd_ = -1, longest_ = 0;
    size_t blockN_ = 8, nAcc_ = 0, skip_ = 0; long blocks_ = 0; int nDb_ = 0, nTx_ = 0, nKer_ = 0; bool haveFloor_ = false, on_ = false, stuckFlagged_ = false, have_ = false, sortedDirty_ = true;
    std::deque<float> hist_; std::vector<double> lens_; std::deque<std::pair<double, double>> recent_;
};
}

std::unique_ptr<Decoder> makePttLog() { return std::make_unique<PttLogDec>(); }

} // namespace dect2::x40
