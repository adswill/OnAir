// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Secondary surveillance radar interrogations on 1030 MHz (ICAO Annex 10 Vol. IV, 3.1.1 and 3.1.2): pulses of 0.8 us in a group.
//   P1 - P3 spacing: Mode 1 3 us, Mode 2 5 us, Mode A (3/A) 8 us, Mode B 17 us, Mode C 21 us, Mode D 25 us; P2 2 us after P1 is the side-lobe suppression pulse (sent on the control antenna);
//   P4 2 us after P3: 0.8 us (Mode A/C-only all-call) or 1.6 us (Mode A/C/S all-call) in an intermode interrogation;
//   Mode S: P1 and P2 2 us apart, then the long pulse P6 (16.25 us for 56 bits, 30.25 us for 112 bits) 2.75 us after the leading edge of P2 (4.75 us after P1).
// The envelope of the 1030 MHz channel (2 Msps and up) is searched for pulses: an adaptive floor (mean of the quiet envelope), a start at 4 times the floor, the pulse's own peak, its leading and trailing edge where the envelope
// crosses half of that peak (interpolated), its width. Pulses of 0.5 - 1.3 us (P1, P2, P3, short P4), 1.3 - 2.1 us (long P4) and 12 - 33 us (P6) are collected into groups that start at a pulse and last 37 us;
// a group is read as an interrogation when the spacings of the pulses to the first one fit one of the patterns above within 0.35 us (0.6 us for P6) and P1 and P3 are within 6 dB of each other.
// The mode is named, P2 is noted (P2 not 9 dB under P1: the interrogation is in a side lobe of the radar and will not be answered), and over 2 s the counts per mode, the measured spacing of each mode (mean, in us) and the
// interrogation rate are shown. A row per interrogation would be hundreds per second: a row per 2 s summarises. It cannot say which radar sent them; the rate and the level are what an antenna sweep (4 to 12 s) shows. Receive only.
#include "x40_dsp.h"
#include <deque>
#include <map>

namespace dect2::x40 {

namespace {
struct Pulse { double lead = 0, width = 0, peak = 0; };
struct Mode { const char* name; double spacing; };
const Mode kModes[] = {{"Mode 1", 3}, {"Mode 2", 5}, {"Mode A", 8}, {"Mode B", 17}, {"Mode C", 21}, {"Mode D", 25}};
constexpr int kNumModes = 6;
constexpr double kGroupUs = 37.0, kTolUs = 0.35;

class SsrDec : public Decoder {
public:
    bool configure(double fs) override { fs_ = fs; dt_ = 1.0 / fs; reset(); return fs >= 1.9e6; }
    void reset() override {
        nf_ = 0; t_ = 0; n_ = 0; inPulse_ = false; peak_ = 0; ring_.assign(kRing, 0.f); rp_ = 0; pulses_.clear(); counts_.clear(); lastRow_ = 0; have_ = false; skip_ = (size_t)(0.002 * fs_); total_ = 0; sm_ = 0;
        winStart_ = 0; winCount_ = 0; res_ = Res(); spacingSum_.clear(); spacingN_.clear(); lvlSum_ = 0; lvlN_ = 0;
    }
    void feed(const cf32* x, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            const float e = std::abs(x[i]);
            if (skip_) { skip_--; nf_ = e; ring_[rp_] = e; rp_ = (rp_ + 1) % kRing; n_++; continue; }
            have_ = true;
            const size_t idx = n_;
            ring_[rp_] = e; rp_ = (rp_ + 1) % kRing; n_++;
            if (!inPulse_) {
                if (e < 3.0f * nf_ || nf_ == 0) nf_ += 0.002f * (e - nf_);                // the floor: the mean of what is quiet
                if (e > 4.0f * std::max(nf_, 1e-9f) && nf_ > 0) { inPulse_ = true; peak_ = e; startIdx_ = idx; lowRun_ = 0; }
            } else {
                peak_ = std::max(peak_, e);
                // the pulse ends when the envelope stays under a quarter of its peak for 0.3 us (a phase reversal in P6 only dips it for less), or after 40 us
                if (e < 0.25f * peak_) lowRun_++; else lowRun_ = 0;
                if ((double)lowRun_ * dt_ >= 0.3e-6 || (lowRun_ >= 1 && dt_ >= 0.3e-6) || (double)(idx - startIdx_) * dt_ > 40e-6) { endPulse(idx); inPulse_ = false; lowRun_ = 0; }
            }
            if ((n_ & 0x3FFF) == 0) { const double now = (double)n_ * dt_; if (now - lastRow_ >= 2.0) summarise(now); }
        }
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        if (!res_.ok) { t.headline = total_ ? fmt("%llu interrogations so far", (unsigned long long)total_) : "no interrogation yet"; return; }
        t.headline = res_.text;
        for (const auto& f : res_.facts) t.facts.push_back(f);
        t.facts.push_back({"interrogations so far", fmt("%llu", (unsigned long long)total_)});
    }
private:
    static constexpr size_t kRing = 4096;
    struct Res { bool ok = false; std::string text; std::vector<std::pair<std::string, std::string>> facts; };
    double at(size_t idx) const { return ring_[(rp_ + kRing - (n_ - idx)) % kRing]; }
    void endPulse(size_t endIdx) {
        // the half-peak crossings: back from the end for the trailing edge, forward from the start for the leading edge
        const double half = 0.5 * peak_;
        const size_t span = endIdx - startIdx_ + 1;
        if (span + 4 >= kRing) return;
        const size_t s0 = startIdx_ > 6 ? startIdx_ - 6 : 0;
        double lead = -1, trail = -1;
        for (size_t k = s0 + 1; k <= endIdx; k++) { const double a = at(k - 1), b = at(k); if (a < half && b >= half) { lead = (double)(k - 1) + (half - a) / (b - a); break; } }
        for (size_t k = endIdx; k > s0; k--) { const double a = at(k - 1), b = at(k); if (a >= half && b < half) { trail = (double)(k - 1) + (a - half) / (a - b); break; } }
        if (lead < 0 || trail < 0 || trail <= lead) return;
        Pulse p; p.lead = lead * dt_ * 1e6; p.width = (trail - lead) * dt_ * 1e6; p.peak = (double)peak_ / std::max((double)nf_, 1e-9);
        if (!((p.width >= 0.45 && p.width <= 2.2) || (p.width >= 12 && p.width <= 33))) return;
        pulses_.push_back(p);
        // an interrogation is read once 37 us have passed since its first pulse
        while (!pulses_.empty() && p.lead - pulses_.front().lead > kGroupUs) { group(); }
    }
    void group() {
        // pulses_[0] is P1; everything within 37 us of it belongs to the group
        const Pulse p1 = pulses_.front(); std::vector<Pulse> g;
        size_t used = 0; for (const Pulse& q : pulses_) { if (q.lead - p1.lead <= kGroupUs) { g.push_back(q); used++; } else break; }
        pulses_.erase(pulses_.begin(), pulses_.begin() + (long)used);
        if (g.size() < 2 || p1.width < 0.45 || p1.width > 1.3) return;
        auto find = [&](double sp, double tol, double wmin, double wmax) -> const Pulse* { for (size_t i = 1; i < g.size(); i++) { if (std::fabs(g[i].lead - p1.lead - sp) <= tol && g[i].width >= wmin && g[i].width <= wmax) return &g[i]; } return nullptr; };
        const Pulse* p2 = find(2.0, kTolUs, 0.45, 1.3);
        const Pulse* p6 = find(4.75, 0.6, 12, 33);
        std::string name; double sp = 0;
        if (p2 && p6) { name = p6->width > 23 ? "Mode S (112 bit)" : "Mode S (56 bit)"; sp = p6->lead - p1.lead; }
        else {
            for (int m = 0; m < kNumModes && name.empty(); m++) {
                const Pulse* p3 = find(kModes[m].spacing, kTolUs, 0.45, 1.3);
                if (!p3) continue;
                const double r = 20 * std::log10(std::max(p1.peak, 1e-9) / std::max(p3->peak, 1e-9)); if (std::fabs(r) > 6) continue;       // main-beam pulses are alike
                name = kModes[m].name; sp = p3->lead - p1.lead;
                if (const Pulse* p4s = find(kModes[m].spacing + 2.0, kTolUs, 0.45, 1.3)) { (void)p4s; name += " + P4 (A/C-only all-call)"; }
                else if (find(kModes[m].spacing + 2.0, kTolUs, 1.3, 2.2)) name += " + P4 (A/C/S all-call)";
                else if (m == 2 || m == 4) {}                                                                                           // plain Mode A / C
            }
        }
        if (name.empty()) return;
        bool supp = false; if (p2 && !p6) supp = p2->peak >= p1.peak * 0.35;                                                          // P2 not 9 dB under P1
        if (p2 && !p6) name += supp ? " (P2: side lobe, suppressed)" : " (P2 low: main beam)";
        total_++; winCount_++; counts_[name]++; spacingSum_[name] += sp; spacingN_[name]++; lvlSum_ += 20 * std::log10(std::max(p1.peak, 1.0)); lvlN_++;
        out->good((float)(20 * std::log10(std::max(p1.peak, 1.0))), 0);
    }
    void summarise(double now) {
        lastRow_ = now;
        while (!pulses_.empty() && now * 1e6 - pulses_.front().lead > 45.0) group();             // groups of the last moments are read too
        if (counts_.empty()) { if (!total_) out->energy(); return; }
        const double span = std::max(now - winStart_, 0.5);
        std::string text, rate; Res r; r.ok = true;
        std::vector<std::pair<int, std::string>> order; for (auto& kv : counts_) order.push_back({kv.second, kv.first}); std::sort(order.rbegin(), order.rend());
        for (auto& o : order) { text += fmt("%s%s: %d (%.2f us)", text.empty() ? "" : "; ", o.second.c_str(), o.first, spacingSum_[o.second] / std::max(1, spacingN_[o.second])); r.facts.push_back({o.second, fmt("%d in %.1f s, spacing %.2f us", o.first, span, spacingSum_[o.second] / std::max(1, spacingN_[o.second]))}); }
        const double lvl = lvlN_ ? lvlSum_ / lvlN_ : 0;
        r.text = fmt("%d interrogations in %.1f s (%.0f per second), P1 about %.0f dB over the floor", winCount_, span, winCount_ / span, lvl);
        r.facts.insert(r.facts.begin(), {"rate", fmt("%.0f interrogations per second", winCount_ / span)});
        res_ = r;
        out->row("Interrogations", fmt("%.0f /s", winCount_ / span), fmt("%s; P1 about %.0f dB over the floor", text.c_str(), lvl), (float)lvl);
        counts_.clear(); spacingSum_.clear(); spacingN_.clear(); winStart_ = now; winCount_ = 0; lvlSum_ = 0; lvlN_ = 0;
    }
    double fs_ = 1, dt_ = 1, t_ = 0, lastRow_ = 0, winStart_ = 0, lvlSum_ = 0; float nf_ = 0, sm_ = 0, peak_ = 0; size_t n_ = 0, startIdx_ = 0, rp_ = 0, skip_ = 0; int winCount_ = 0, lvlN_ = 0, lowRun_ = 0; unsigned long long total_ = 0; bool inPulse_ = false, have_ = false;
    std::vector<float> ring_; std::vector<Pulse> pulses_; std::map<std::string, int> counts_; std::map<std::string, double> spacingSum_; std::map<std::string, int> spacingN_; Res res_;
};
}

std::unique_ptr<Decoder> makeSsrInt() { return std::make_unique<SsrDec>(); }

} // namespace dect2::x40
