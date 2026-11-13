// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RC model transmitters, PPM frames (26 / 27 / 35 / 40 / 72 MHz, AM or FM): the channel values of the frame. The transmitter keys its carrier
// (AM) or shifts it (FM) for a short pulse before each channel; the time from one pulse to the next is that channel (about 1.0 to 2.0 ms, 1.5 ms
// at centre), and one long gap (the sync, more than 3.5 ms) closes a frame of 18 to 25 ms. The pulse is taken as the state the carrier spends
// less time in, so inverted-polarity sets work too. Receive only: the stick values are shown, nothing is sent.
#include "x40_int.h"
#include <deque>

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 160000, kCut = 40000;
constexpr double kMinCh = 0.55e-3, kMaxCh = 2.7e-3, kSyncMin = 3.5e-3, kSyncMax = 40e-3;

class RcPpmDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, kCut)) return false;
        rate_ = dec_.outRate();
        reset();
        return true;
    }
    void reset() override {
        dec_.reset(); hi_ = 0; lo_ = 1e9f; sm_ = 0; state_ = 0; t_ = 0; tLast_ = -1; chans_.clear();
        timeIn_[0] = timeIn_[1] = 0; lastN_ = 0; stable_ = 0; lastRowT_ = -1e9; pulseState_ = 1; inited_ = false;
    }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) { step(std::abs(a)); });
    }
    void fill(X40Telemetry& t) const override {
        t.headline = frames_ ? fmt("%d channel%s, frame %.1f ms", lastN_, lastN_ == 1 ? "" : "s", lastFrameMs_) : "no PPM frame yet";
        if (frames_) {
            t.facts.push_back({"channels", fmt("%d", lastN_)});
            t.facts.push_back({"frame length", fmt("%.2f ms", lastFrameMs_)});
            t.facts.push_back({"frame rate", fmt("%.1f Hz", 1000.0 / std::max(1.0, lastFrameMs_))});
            t.facts.push_back({"pulse state", pulseState_ ? "carrier low (AM notch or FM shift)" : "carrier high"});
            for (size_t i = 0; i < last_.size(); i++) t.facts.push_back({fmt("channel %zu", i + 1), fmt("%.3f ms  %+.0f %%", last_[i] * 1e3, (last_[i] * 1e3 - 1.5) * 200.0)});
        }
    }
private:
    void step(float e) {
        t_ += 1.0 / rate_;
        sm_ += 0.12f * (e - sm_);
        // peak and floor trackers: fast one way, slow the other
        const float dn = 1.0f / (float)(rate_ * 0.05);
        hi_ = sm_ > hi_ ? hi_ + 0.06f * (sm_ - hi_) : hi_ - dn * (hi_ - sm_);
        lo_ = sm_ < lo_ ? lo_ - 0.06f * (lo_ - sm_) : lo_ + dn * (sm_ - lo_);
        const float span = hi_ - lo_;
        // hysteresis around the middle; no decision while the swing is tiny (no signal, or noise only)
        const bool live = span > 0.25f * hi_ && hi_ > 1e-6f;
        if (!live) { if (!state_ && ++idle_ > rate_ * 0.1) { chans_.clear(); tLast_ = -1; } return; }
        idle_ = 0;
        const float mid = lo_ + 0.5f * span, h = 0.12f * span;
        const int ns = state_ ? (sm_ < mid - h ? 0 : 1) : (sm_ > mid + h ? 1 : 0);
        timeIn_[ns] = timeIn_[ns] * (1 - 1 / (rate_ * 0.5)) + 1.0 / rate_;
        if (inited_ && ns != state_) {
            state_ = ns;
            pulseState_ = timeIn_[0] < timeIn_[1] ? 0 : 1;
            if (ns == pulseState_) onset();
        }
        if (!inited_) { state_ = ns; inited_ = true; }
    }
    void onset() {
        out->energy();
        if (tLast_ >= 0) {
            const double d = t_ - tLast_;
            if (d >= kSyncMin && d <= kSyncMax) endFrame(d);
            else if (d >= kMinCh && d <= kMaxCh) { chans_.push_back(d); if (chans_.size() > 16) chans_.clear(); }
            else { chans_.clear(); if (d < kSyncMax) out->bad(); }
        }
        tLast_ = t_;
    }
    void endFrame(double sync) {
        const int n = (int)chans_.size();
        if (n >= 2 && n <= 12) {
            double sum = sync; for (double c : chans_) sum += c;
            lastFrameMs_ = sum * 1e3; last_ = chans_; frames_++;
            stable_ = (n == lastN_) ? stable_ + 1 : 0; lastN_ = n;
            if (stable_ >= 1) {
                out->good(20 * std::log10(std::max(1.f, hi_ / std::max(lo_, 1e-9f))), 0);
                if (t_ - lastRowT_ >= 0.5) {
                    lastRowT_ = t_;
                    std::string s;
                    for (double c : chans_) s += fmt("%.2f ", c * 1e3);
                    out->row("PPM frame", fmt("%d ch", n), fmt("%s ms, frame %.1f ms", s.c_str(), lastFrameMs_), toDb(hi_ * hi_));
                }
            }
        } else if (n) out->bad();
        chans_.clear();
    }
    Decimator dec_;
    double rate_ = 1, t_ = 0, tLast_ = -1, timeIn_[2] = {0, 0}, lastFrameMs_ = 0, lastRowT_ = -1e9;
    float hi_ = 0, lo_ = 1e9f, sm_ = 0;
    int state_ = 0, pulseState_ = 1, lastN_ = 0, stable_ = 0; bool inited_ = false;
    double idle_ = 0; uint64_t frames_ = 0;
    std::vector<double> chans_, last_;
};
}

std::unique_ptr<Decoder> makeRcPpm() { return std::make_unique<RcPpmDec>(); }

} // namespace dect2::x40
