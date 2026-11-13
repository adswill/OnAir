// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ILS marker beacon receiver (75 MHz, ICAO Annex 10 Vol. I, 3.1.7 and 3.1.7.6): the carrier is amplitude modulated with a keyed tone, one tone and one keying per marker:
//   outer marker   400 Hz, dashes, 2 per second (continuously)
//   middle marker 1300 Hz, alternate dots and dashes, 95 dot-dash pairs per minute
//   inner marker  3000 Hz, dots, 6 per second
// (the inner marker tone and keying are also those of an airways / fan marker's dots; the decoder reads what it hears). The envelope of the 12 kHz channel is cut into 10 ms blocks; a Goertzel filter at each of the three tones
// gives the depth of that tone in percent of the carrier; a block is keyed when one tone has at least 30 % depth and at least three times the depth of each other tone. Runs of keyed blocks are the elements, runs of unkeyed blocks the gaps.
// A passage starts at the first keyed block and ends after 1.5 s without one. The marker is named from its tone (within 10 %), the keying (elements per second, dot / dash mix) is measured and compared with the standard:
// a tone with the wrong keying is listed as "tone only, keying does not fit". Receive only; it names the marker, it is not a position fix (the marker is a few hundred metres wide on the ground).
#include "x40_dsp.h"
#include <map>

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 12000, kCut = 5000;
constexpr int kBlock = 120;                        // 10 ms at 12 kHz
constexpr double kTones[3] = {400, 1300, 3000};
const char* const kNames[3] = {"Outer marker", "Middle marker", "Inner marker"};
constexpr double kMinDepth = 0.30, kDominance = 3.0, kEndSec = 1.5;

class MarkerDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, std::min(kCut, 0.45 * fs / d))) return false;
        rate_ = dec_.outRate();
        for (int k = 0; k < 3; k++) { const double w = 2 * kPi * kTones[k] / rate_; coef_[k] = 2.0 * std::cos(w); }
        reset(); return true;
    }
    void reset() override {
        dec_.reset(); n_ = 0; mean_ = 0; for (int k = 0; k < 3; k++) s1_[k] = s2_[k] = 0; blocks_ = 0; skip_ = (size_t)(0.3 * rate_);
        tone_ = -1; run_ = 0; keyed_ = false; ons_.clear(); offs_.clear(); last_ = -1e9; t0_ = 0; depthSum_ = 0; depthN_ = 0; passages_ = 0; res_ = Res(); have_ = false; unkeyed_ = 0;
    }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) {
            const float e = std::abs(a);
            if (skip_) { skip_--; mean_ = e; return; }
            mean_ += 0.0005f * (e - mean_);                             // the carrier level: 0.2 s time constant
            const double v = e - mean_;
            for (int k = 0; k < 3; k++) { const double s0 = v + coef_[k] * s1_[k] - s2_[k]; s2_[k] = s1_[k]; s1_[k] = s0; }
            if (++n_ == kBlock) { block(); n_ = 0; for (int k = 0; k < 3; k++) s1_[k] = s2_[k] = 0; }
        });
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        if (!res_.ok) { t.headline = passages_ ? fmt("%d marker passage(s) so far", passages_) : "no marker tone"; return; }
        t.headline = fmt("%s: %.0f Hz, %s", res_.name.c_str(), res_.hz, res_.keying.c_str());
        t.facts.push_back({"marker", res_.name});
        t.facts.push_back({"tone", fmt("%.0f Hz, depth %.0f %% of the carrier", res_.hz, res_.depth * 100)});
        t.facts.push_back({"keying", res_.keying});
        t.facts.push_back({"keying fits the standard", res_.fits ? "yes" : "no"});
        t.facts.push_back({"last passage", fmt("%.1f s", res_.dur)});
        t.facts.push_back({"passages", fmt("%d", passages_)});
    }
private:
    struct Res { bool ok = false, fits = false; std::string name, keying; double hz = 0, depth = 0, dur = 0; };
    void block() {
        have_ = true; blocks_++;
        double dep[3]; const double m = std::max((double)mean_, 1e-9);
        for (int k = 0; k < 3; k++) { const double p = s1_[k] * s1_[k] + s2_[k] * s2_[k] - coef_[k] * s1_[k] * s2_[k]; dep[k] = 2.0 * std::sqrt(std::max(p, 0.0)) / kBlock / m; }   // amplitude of the tone over the carrier
        int best = 0; for (int k = 1; k < 3; k++) if (dep[k] > dep[best]) best = k;
        bool key = dep[best] >= kMinDepth; for (int k = 0; k < 3; k++) if (k != best && dep[k] * kDominance > dep[best]) key = false;
        const double t = blocks_ * (double)kBlock / rate_;
        if (key) {
            if (tone_ < 0) { tone_ = best; t0_ = t; ons_.clear(); offs_.clear(); run_ = 0; keyed_ = false; depthSum_ = 0; depthN_ = 0; }
            if (best != tone_) { end(t); tone_ = best; t0_ = t; run_ = 0; keyed_ = false; depthSum_ = 0; depthN_ = 0; }   // another tone: a new passage
            if (!keyed_) { if (run_ > 0 && !ons_.empty()) offs_.push_back(run_); keyed_ = true; run_ = 0; }
            run_++; depthSum_ += dep[best]; depthN_++; last_ = t; unkeyed_ = 0; out->energy();
        } else if (tone_ >= 0) {
            if (keyed_) { ons_.push_back(run_); keyed_ = false; run_ = 0; }
            run_++; unkeyed_++;
            if (t - last_ > kEndSec) end(last_);
        }
    }
    void end(double tEnd) {
        if (tone_ < 0) return;
        if (keyed_) { ons_.push_back(run_); keyed_ = false; }
        const int tone = tone_; tone_ = -1; run_ = 0;
        if (ons_.size() < 3) { ons_.clear(); offs_.clear(); return; }                   // a blip, not a marker
        const double dur = tEnd - t0_ + 0.01, blk = (double)kBlock / rate_;
        // elements: a dash is at least 1.8 times the shortest element of the passage when both kinds are present
        std::vector<int> on = ons_; std::sort(on.begin(), on.end());
        const double shortest = on.front(), longest = on.back();
        const bool mixed = longest >= 1.8 * shortest;
        const double thr = std::sqrt(shortest * longest);
        int dots = 0, dashes = 0; for (int v : ons_) { if (mixed && v >= thr) dashes++; else if (mixed) dots++; else dots++; }
        const double rate = (double)ons_.size() / std::max(dur - (offs_.empty() ? 0.0 : 0.0), 0.5);
        const double dashLen = longest * blk, dotLen = shortest * blk;
        Res r; r.ok = true; r.hz = kTones[tone]; r.name = kNames[tone]; r.depth = depthN_ ? depthSum_ / depthN_ : 0; r.dur = dur;
        bool fits = false;
        if (tone == 0) { fits = !mixed && rate >= 1.4 && rate <= 2.8 && dotLen >= 0.15; r.keying = fmt("%.1f dashes per second (%.2f s long)", rate, dotLen); }
        else if (tone == 2) { fits = !mixed && rate >= 4.5 && rate <= 7.5 && dotLen <= 0.14; r.keying = fmt("%.1f dots per second", rate); }
        else {
            // middle marker: dot-dash pairs; the pair rate from the number of dashes and dots, 95 per minute = 1.58 per second
            const double pairs = std::min(dots, dashes) / std::max(dur - 0.3, 0.5);
            fits = mixed && std::abs(dots - dashes) <= 1 && pairs * 60 >= 75 && pairs * 60 <= 115 && dashLen < 0.6;
            r.keying = fmt("%.0f dot-dash pairs per minute (dot %.2f s, dash %.2f s)", pairs * 60, dotLen, dashLen);
        }
        r.fits = fits;
        res_ = r; passages_++;
        out->good(0, 0);
        out->row(fits ? "Marker" : "Tone only", fmt("%s", r.name.c_str()), fmt("%.0f Hz, %s, depth %.0f %%, %.1f s%s", r.hz, r.keying.c_str(), r.depth * 100, dur, fits ? "" : "; the keying does not fit the standard"), (float)toDb((float)(r.depth * r.depth)));
        ons_.clear(); offs_.clear();
    }
    Decimator dec_; double rate_ = 1, coef_[3] = {}, s1_[3] = {}, s2_[3] = {}, last_ = -1e9, t0_ = 0, depthSum_ = 0; float mean_ = 0;
    int n_ = 0, tone_ = -1, run_ = 0, unkeyed_ = 0, depthN_ = 0, passages_ = 0; long blocks_ = 0; size_t skip_ = 0; bool keyed_ = false, have_ = false;
    std::vector<int> ons_, offs_; Res res_;
};
}

std::unique_ptr<Decoder> makeMarker() { return std::make_unique<MarkerDec>(); }

} // namespace dect2::x40
