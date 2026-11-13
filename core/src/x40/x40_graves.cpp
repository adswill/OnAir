// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GRAVES space-surveillance radar (143.050 MHz, a continuous unmodulated carrier from France) and meteor echoes. A meteor trail reflects the carrier with a small
// Doppler shift for a fraction of a second to a few seconds. The carrier is the strongest steady line of the averaged spectrum (it follows the radio's
// frequency error); an echo is a second peak near it that stands well above the noise floor in at least two spectra in a row. Receive only.
#include "x40_int.h"

namespace dect2::x40 {

namespace {
constexpr int kN = 1024;
constexpr double kWorkTarget = 2400, kCut = 1000;
constexpr float kThrDb = 12.f;
constexpr int kGuard = 5, kTrack = 4, kMissMax = 2;

void fft(std::vector<cf32>& a) {
    const int n = (int)a.size();
    for (int i = 1, j = 0; i < n; i++) { int bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2 * kPi / len;
        const cf32 wl((float)std::cos(ang), (float)std::sin(ang));
        for (int i = 0; i < n; i += len) {
            cf32 w(1, 0);
            for (int k = 0; k < len / 2; k++) { const cf32 u = a[i + k], v = a[i + k + len / 2] * w; a[i + k] = u + v; a[i + k + len / 2] = u - v; w *= wl; }
        }
    }
}

class GravesDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, kCut)) return false;
        rate_ = dec_.outRate(); binHz_ = rate_ / kN;
        win_.resize(kN); for (int i = 0; i < kN; i++) { const double x = 2 * kPi * i / (kN - 1); win_[i] = (float)(0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x)); }   // Blackman-Harris: the strong carrier leaves no sidelobes to mistake for an echo
        reset();
        return true;
    }
    void reset() override {
        dec_.reset(); buf_.assign(kN, cf32(0, 0)); fillN_ = 0; avg_.assign(kN, 0.f); avgN_ = 0; ev_ = Event(); t_ = 0; frames_ = 0;
    }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) {
            buf_[fillN_++] = a;
            if (fillN_ == kN) {
                frame();
                std::copy(buf_.begin() + kN / 2, buf_.end(), buf_.begin());   // 50 % overlap
                fillN_ = kN / 2;
            }
        });
    }
    void fill(X40Telemetry& t) const override {
        t.cfoHz = carrierHz_;
        t.headline = !frames_ ? "waiting for data" : carrierOk_ ? fmt("carrier %+.1f Hz, %d echoes", carrierHz_, echoes_) : "no carrier found";
        if (carrierOk_) {
            t.facts.push_back({"carrier offset", fmt("%+.1f Hz from the tuned frequency", carrierHz_)});
            t.facts.push_back({"carrier above noise", fmt("%.1f dB", carrierDb_)});
            t.facts.push_back({"echoes", fmt("%d", echoes_)});
            t.facts.push_back({"frequency resolution", fmt("%.2f Hz", binHz_)});
        }
    }
private:
    struct Event { bool open = false; int frames = 0, miss = 0, bin = 0; double dopSum = 0, first = 0, firstDop = 0, lastDop = 0; float maxDb = -200; };
    void frame() {
        std::vector<cf32> a(kN);
        for (int i = 0; i < kN; i++) a[i] = buf_[i] * win_[i];
        fft(a);
        std::vector<float> p(kN);
        for (int i = 0; i < kN; i++) p[i] = std::norm(a[i]);
        t_ += kN / 2 / rate_; frames_++;
        // the average spectrum picks the carrier: echoes come and go, the carrier stays
        const float al = avgN_ < 20 ? 1.f / (avgN_ + 1) : 1.f / 20; avgN_++;
        for (int i = 0; i < kN; i++) avg_[i] += al * (p[i] - avg_[i]);
        int cb = 0; for (int i = 1; i < kN; i++) if (avg_[i] > avg_[cb]) cb = i;
        std::vector<float> s(p); std::nth_element(s.begin(), s.begin() + kN / 2, s.end());
        const float med = std::max(s[kN / 2], 1e-20f);
        const float noise = med / 0.693f;                      // mean of a noise-only bin
        carrierDb_ = toDb(avg_[cb] / noise);
        carrierOk_ = avgN_ >= 4 && carrierDb_ > 15.f;
        auto sbin = [&](int b) { return b >= kN / 2 ? b - kN : b; };
        carrierHz_ = sbin(cb) * binHz_;
        if (!carrierOk_) { closeEvent(false); return; }
        out->energy();
        // the strongest bin outside the carrier's skirt
        int pb = -1; float pv = 0;
        for (int i = 0; i < kN; i++) {
            int d = sbin(i) - sbin(cb); if (d > kN / 2) d -= kN; if (d < -kN / 2) d += kN;
            if (std::abs(d) <= kGuard) continue;
            if (p[i] > pv) { pv = p[i]; pb = i; }
        }
        const float db = toDb(pv / noise);
        const bool hit = pb >= 0 && db > kThrDb;
        if (hit) {
            int d = sbin(pb) - sbin(cb); if (d > kN / 2) d -= kN; if (d < -kN / 2) d += kN;
            const double dop = d * binHz_;
            if (ev_.open && std::abs(d - ev_.bin) <= kTrack) {
                ev_.frames++; ev_.miss = 0; ev_.bin = d; ev_.dopSum += dop; ev_.lastDop = dop; ev_.maxDb = std::max(ev_.maxDb, db);
            } else {
                closeEvent(true);
                ev_ = Event(); ev_.open = true; ev_.frames = 1; ev_.bin = d; ev_.dopSum = dop; ev_.first = t_; ev_.firstDop = ev_.lastDop = dop; ev_.maxDb = db;
            }
        } else if (ev_.open && ++ev_.miss > kMissMax) closeEvent(true);
    }
    void closeEvent(bool) {
        if (!ev_.open) return;
        if (ev_.frames >= 2) {
            const double dur = ev_.frames * (kN / 2) / rate_ + (kN / 2) / rate_;
            const double drift = (ev_.lastDop - ev_.firstDop) / std::max(0.2, dur);
            echoes_++;
            out->good(ev_.maxDb, carrierHz_);
            out->row("Meteor echo", fmt("%+.0f Hz", ev_.dopSum / ev_.frames), fmt("%.1f dB above noise, %.1f s, drift %+.0f Hz/s", ev_.maxDb, dur, drift), ev_.maxDb);
        }
        ev_ = Event();
    }
    Decimator dec_;
    std::vector<cf32> buf_; std::vector<float> win_, avg_;
    int fillN_ = 0, avgN_ = 0, echoes_ = 0; uint64_t frames_ = 0;
    double rate_ = 1, binHz_ = 1, t_ = 0, carrierHz_ = 0; float carrierDb_ = 0; bool carrierOk_ = false;
    Event ev_;
};
}

std::unique_ptr<Decoder> makeGraves() { return std::make_unique<GravesDec>(); }

} // namespace dect2::x40
