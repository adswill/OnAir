// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Spectrum occupancy monitor: the whole captured span is cut into 25 kHz channels; every 10 ms each channel is judged busy or idle against a noise floor that follows the
// quietest fifth of the channels, with hysteresis (on at 6 dB over the floor, off below 4 dB). Per channel: the share of the time it was busy (duty cycle), the number of
// transmissions and the longest one; the band's occupancy; every finished transmission is logged with its length and strength. It measures what arrives at this radio in this
// place: a channel that is busy in one place can be empty in another, and a very wide signal makes all its channels busy. If more than four fifths of the span is busy
// the floor is read too high and weak signals are missed. Receive only.
#include "x40_dsp.h"

namespace dect2::x40 {

namespace {
constexpr int kFft = 2048;
constexpr double kChanHz = 25000, kDecisionSec = 0.01, kOnDb = 6.0, kOffDb = 4.0, kMinLogSec = 0.05, kEdge = 0.45;
constexpr int kOnCount = 3, kOffCount = 5, kWarmup = 30;

struct Chan {
    bool busy = false; int on = 0, off = 0; double start = 0, busySec = 0, longest = 0, peakOver = 0; int events = 0;
};

class OccupancyDec : public Decoder {
public:
    bool configure(double fs) override {
        fs_ = fs;
        binHz_ = fs / kFft;
        chBins_ = std::max(2, (int)std::lround(kChanHz / binHz_));
        nCh_ = std::max(1, (int)std::floor(2 * kEdge * kFft / chBins_));
        if (nCh_ < 3) return false;
        map_.assign(kFft, -1);
        for (int k = 0; k < kFft; k++) {
            const int j = k < kFft / 2 ? k : k - kFft;
            if (j >= -1 && j <= 1) continue;                                            // the radio's own DC spike
            const int c = (int)std::floor((j + nCh_ * chBins_ / 2.0) / chBins_);
            if (c >= 0 && c < nCh_) map_[k] = c;
        }
        cnt_.assign(nCh_, 0);
        for (int k = 0; k < kFft; k++) if (map_[k] >= 0) cnt_[map_[k]]++;
        win_.resize(kFft); for (int i = 0; i < kFft; i++) win_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kFft));
        frames_ = std::max(1, (int)std::lround(kDecisionSec * fs / kFft));
        reset(); return true;
    }
    void reset() override {
        ch_.assign(nCh_, Chan()); acc_.assign(nCh_, 0.0); fill_ = 0; nf_ = 0; t_ = 0; decisions_ = 0; floorLin_ = 0; busyNow_ = 0; buf_.clear(); haveFloor_ = false;
        activeSec_ = 0; floorDb_ = -120;
    }
    void setCenterHz(double hz) override { centre_ = hz; }
    void feed(const cf32* x, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            buf_.push_back(x[i]);
            if ((int)buf_.size() == kFft) { frame(); buf_.clear(); }
        }
    }
    void fill(X40Telemetry& t) const override {
        if (decisions_ < kWarmup) { t.headline = "measuring the noise floor"; return; }
        int busy = 0; double occ = 0; for (const Chan& c : ch_) { busy += c.busy; occ += c.busySec; }
        const double el = t_;
        t.headline = fmt("%d of %d channels busy now, band occupancy %.1f %% over %.0f s", busy, nCh_, el > 0 ? 100 * occ / (el * nCh_) : 0.0, el);
        t.facts.push_back({"channels", fmt("%d of %.1f kHz over %.3f MHz", nCh_, chBins_ * binHz_ / 1000, nCh_ * chBins_ * binHz_ / 1e6)});
        t.facts.push_back({"noise floor", fmt("%.1f dB (relative, per bin); busy above +%.0f dB", floorDb_, kOnDb)});
        t.facts.push_back({"busy now", fmt("%d channels", busy)});
        t.facts.push_back({"band occupancy", fmt("%.1f %% of channel-time", el > 0 ? 100 * occ / (el * nCh_) : 0.0)});
        std::vector<int> idx(nCh_); for (int i = 0; i < nCh_; i++) idx[i] = i;
        std::sort(idx.begin(), idx.end(), [&](int a, int b) { return ch_[a].busySec > ch_[b].busySec; });
        for (int r = 0; r < 5 && r < nCh_; r++) {
            const Chan& c = ch_[idx[r]]; if (c.busySec <= 0) break;
            t.facts.push_back({fmt("busiest %d", r + 1), fmt("%s: busy %.1f %% of the time, %d transmissions, longest %.1f s", freqStr(idx[r]).c_str(), el > 0 ? 100 * c.busySec / el : 0.0, c.events, c.longest)});
        }
    }
private:
    double chanFreq(int c) const { return centre_ + (c - nCh_ / 2.0 + 0.5) * chBins_ * binHz_; }
    std::string freqStr(int c) const {
        const double f = chanFreq(c);
        if (centre_ > 0) return fmt("%.4f MHz", f / 1e6);
        return fmt("%+.1f kHz", (c - nCh_ / 2.0 + 0.5) * chBins_ * binHz_ / 1000);
    }
    void frame() {
        std::vector<cf32> a(kFft);
        for (int i = 0; i < kFft; i++) a[i] = buf_[i] * win_[i];
        fftx(a);
        for (int k = 0; k < kFft; k++) if (map_[k] >= 0) acc_[map_[k]] += std::norm(a[k]);
        if (++fill_ < frames_) return;
        const double dt = (double)frames_ * kFft / fs_;
        t_ += dt; decisions_++;
        std::vector<double> p(nCh_);
        for (int c = 0; c < nCh_; c++) { p[c] = acc_[c] / ((double)fill_ * cnt_[c] * kFft) + 1e-18; acc_[c] = 0; }
        fill_ = 0;
        std::vector<double> s(p); std::sort(s.begin(), s.end());
        const double q = s[(size_t)(0.2 * (nCh_ - 1))];
        if (!haveFloor_) { floorLin_ = q; haveFloor_ = true; } else floorLin_ = std::exp(0.97 * std::log(floorLin_) + 0.03 * std::log(q));
        floorDb_ = (float)(10 * std::log10(floorLin_));
        const double on = floorLin_ * std::pow(10.0, kOnDb / 10), off = floorLin_ * std::pow(10.0, kOffDb / 10);
        int busy = 0;
        for (int c = 0; c < nCh_; c++) {
            Chan& h = ch_[c];
            const bool above = h.busy ? p[c] > off : p[c] > on;
            if (decisions_ < kWarmup) continue;
            if (above) { h.on++; h.off = 0; } else { h.off++; h.on = 0; }
            const double over = 10 * std::log10(p[c] / floorLin_);
            if (!h.busy && h.on >= kOnCount) { h.busy = true; h.start = t_ - kOnCount * dt; h.peakOver = over; h.busySec += kOnCount * dt; }
            else if (h.busy) {
                if (above) { h.busySec += dt; h.peakOver = std::max(h.peakOver, over); }
                else if (h.off >= kOffCount) {
                    h.busy = false; h.busySec -= (kOffCount - 1) * dt;      // the idle decisions counted while waiting
                    const double len = t_ - h.start - kOffCount * dt;
                    h.events++; h.longest = std::max(h.longest, len);
                    if (len >= kMinLogSec) out->row("Activity", freqStr(c), fmt("busy %.2f s, %.1f dB over the floor", len, h.peakOver), (float)(10 * std::log10(p[c])));
                } else h.busySec += dt;
            }
            busy += h.busy;
        }
        busyNow_ = busy;
        if (busy) { out->good(0, 0); activeSec_ = t_; } else out->energy();
    }
    double fs_ = 0, binHz_ = 1, centre_ = 0, t_ = 0, floorLin_ = 0, activeSec_ = 0; int chBins_ = 2, nCh_ = 0, frames_ = 1, fill_ = 0, nf_ = 0, decisions_ = 0, busyNow_ = 0; bool haveFloor_ = false; float floorDb_ = -120;
    std::vector<int> map_, cnt_; std::vector<float> win_; std::vector<double> acc_; std::vector<Chan> ch_; std::vector<cf32> buf_;
};
}

std::unique_ptr<Decoder> makeOccupancy() { return std::make_unique<OccupancyDec>(); }

} // namespace dect2::x40
