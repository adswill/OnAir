// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Autonomous airband scanning: see airband_auto.h.
#include "dect2/airband/airband_auto.h"
#include "dect2/airband/airband_store.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dect2 {
namespace {
constexpr size_t kLearnMax = 512;
constexpr double kNeighbourHz = 9000, kNeighbourDb = 12;   // a carrier this much weaker, this close to an active one is its splatter
constexpr float kHotScore = 3.f;
bool same(double a, double b) { return std::fabs(a - b) < 600; }
}   // namespace

// ---------------------------------------------------------------- AirbandAuto

std::vector<double> AirbandAuto::guardList() { return {121.500e6, 243.000e6}; }
bool AirbandAuto::isGuard(double f) { for (double g : guardList()) if (std::fabs(f - g) < 600) return true; return false; }

AirbandLearned* AirbandAuto::find(double f) { for (auto& l : learn_) if (same(l.freqHz, f)) return &l; return nullptr; }
const AirbandLearned* AirbandAuto::find(double f) const { for (const auto& l : learn_) if (same(l.freqHz, f)) return &l; return nullptr; }
bool AirbandAuto::continuous(double f) const { const AirbandLearned* l = find(f); return l && l->continuous; }
float AirbandAuto::score(double f) const { const AirbandLearned* l = find(f); return l ? l->score : 0.f; }

void AirbandAuto::markContinuous(double f) {
    AirbandLearned* l = find(f);
    if (!l) { learn_.push_back({f, 0.f, 0, false}); l = &learn_.back(); }
    l->continuous = true;
}

void AirbandAuto::noteTransmission(double f, double dur) {
    if (!std::isfinite(f)) return;
    AirbandLearned* l = find(f);
    if (!l) {
        if (learn_.size() >= kLearnMax) {   // the least heard goes
            size_t w = 0;
            for (size_t i = 1; i < learn_.size(); i++) if (learn_[i].score < learn_[w].score) w = i;
            learn_.erase(learn_.begin() + (long)w);
        }
        learn_.push_back({f, 0.f, 0, false});
        l = &learn_.back();
    }
    l->opens++;
    l->score = std::min(100.f, l->score + 1.f + (float)std::min(dur, 10.0) * 0.5f);
    if (dur < 20) l->continuous = false;   // it was keyed after all
}

AirbandAutoStep AirbandAuto::step(double now, const std::vector<AirbandSeen>& seen, const std::vector<AirbandAutoView>& cur, double lo, double hi) {
    AirbandAutoStep out;
    for (auto& l : learn_) l.score *= 0.9995f;   // fades over about an hour of 0.25 s steps
    // 1. channels that go
    std::vector<AirbandAutoView> keep;
    for (const auto& v : cur) {
        const bool guard = isGuard(v.freqHz);
        const double lastSign = std::max(v.lastEndSec, v.madeSec);
        const double idle = now - lastSign;
        const double hold = v.pinned ? (v.lastEndSec >= 0 ? kKeepSec : 20.0) : (score(v.freqHz) >= 5.f ? kKeepHotSec : kKeepSec);
        if (!v.active && !(guard && on_) && idle > hold) out.drop.push_back(v.freqHz);
        else keep.push_back(v);
    }
    if (!on_) return out;
    // 2. carriers that key up
    auto have = [&](double f) { for (const auto& v : keep) if (same(v.freqHz, f)) return true; return false; };
    std::vector<const AirbandSeen*> cand;
    for (const auto& s : seen) {
        if (!s.activeNow || s.freqHz < lo || s.freqHz > hi || have(s.freqHz)) continue;
        const bool hot = score(s.freqHz) >= kHotScore || isGuard(s.freqHz);
        if (s.hits < (hot ? 1u : 2u)) continue;
        cand.push_back(&s);
    }
    std::sort(cand.begin(), cand.end(), [](const AirbandSeen* a, const AirbandSeen* b) { return a->excessDb > b->excessDb; });
    std::vector<const AirbandSeen*> taken;
    for (const AirbandSeen* c : cand) {
        // splatter or a heterodyne of a much stronger neighbour
        bool dup = false;
        for (const AirbandSeen* t : taken) if (std::fabs(t->freqHz - c->freqHz) < kNeighbourHz && t->excessDb >= c->excessDb + kNeighbourDb) dup = true;
        for (const auto& v : keep) if (v.active && std::fabs(v.freqHz - c->freqHz) < kNeighbourHz && v.peakExcessDb >= c->excessDb + kNeighbourDb) dup = true;
        if (dup) continue;
        taken.push_back(c);
    }
    size_t n = keep.size();
    auto room = [&]() {
        if (n < kAirbandAutoMax) return true;
        // the quietest idle one makes room
        int w = -1; double wt = 1e30;
        for (size_t i = 0; i < keep.size(); i++) {
            const auto& v = keep[i];
            if (v.active || v.pinned || isGuard(v.freqHz)) continue;
            bool dropped = false;
            for (double d : out.drop) dropped |= same(d, v.freqHz);
            if (dropped) continue;
            const double t = std::max(v.lastEndSec, v.madeSec);
            if (now - t < 5) continue;
            if (t < wt) { wt = t; w = (int)i; }
        }
        if (w < 0) return false;
        out.drop.push_back(keep[(size_t)w].freqHz);
        keep.erase(keep.begin() + w);
        n--;
        return true;
    };
    for (const AirbandSeen* c : taken) {
        if (!room()) break;
        AirbandChannel ch; ch.freqHz = c->freqHz; ch.is833 = c->is833; ch.label = "auto"; ch.priority = isGuard(c->freqHz);
        out.add.push_back(ch);
        AirbandAutoView v; v.freqHz = c->freqHz; v.madeSec = now; keep.push_back(v);
        n++;
    }
    // 3. the guard channels are always open
    for (double g : guardList()) {
        if (g < lo || g > hi || have(g)) continue;
        bool queued = false;
        for (const auto& a : out.add) queued |= same(a.freqHz, g);
        if (queued) continue;
        if (!room()) break;
        AirbandChannel ch; ch.freqHz = g; ch.is833 = false; ch.label = "guard"; ch.priority = true;
        out.add.push_back(ch);
        n++;
    }
    return out;
}

std::string AirbandAuto::save() const { return airbandLearnedEncode(learn_); }

bool AirbandAuto::load(const std::string& s) {
    if (s.compare(0, 3, "l1:") != 0) return false;
    learn_ = airbandLearnedDecode(s);
    return true;
}

// ---------------------------------------------------------------- AirbandHopper

void AirbandHopper::plan(double span) {
    const double bw = kBandHiHz - kBandLoHz;
    const int n = span <= 0 ? 1 : std::max(1, (int)std::ceil(bw / span - 1e-9));
    const double key = n;
    if (key == spanKey_ && grp_.size() == (size_t)n) return;
    spanKey_ = key;
    grp_.assign((size_t)n, G());
    const double w = bw / n;
    for (int i = 0; i < n; i++) {
        G& g = grp_[(size_t)i];
        g.lo = kBandLoHz + i * w; g.hi = g.lo + w; g.center = 0.5 * (g.lo + g.hi);
    }
    cur_ = 0; arrive_ = 0; waiting_ = false;
}

void AirbandHopper::seed(const std::vector<AirbandLearned>& l) {
    for (auto& g : grp_) g.act = 0;
    for (const auto& x : l) {
        for (auto& g : grp_) if (x.freqHz >= g.lo && x.freqHz < g.hi) g.act += std::min(x.score, 10.f) * 0.2;
    }
}

void AirbandHopper::locate(double centerHz, double clock) {
    if (grp_.empty()) return;
    size_t best = 0;
    for (size_t i = 1; i < grp_.size(); i++) if (std::fabs(grp_[i].center - centerHz) < std::fabs(grp_[best].center - centerHz)) best = i;
    cur_ = best; arrive_ = clock; waiting_ = false; lastOpen_ = -1e9;
    grp_[cur_].heard = false;
}

void AirbandHopper::noteActivity(double f, double clock) {
    (void)clock;
    for (size_t i = 0; i < grp_.size(); i++) {
        if (f >= grp_[i].lo - 1 && f < grp_[i].hi + 1) { grp_[i].act += 1; if (i == cur_) grp_[i].heard = true; break; }
    }
}

double AirbandHopper::dwellSec() const {
    if (cur_ >= grp_.size()) return kMinDwell;
    const G& g = grp_[cur_];
    return std::min(kMaxDwell, kMinDwell + 1.5 * std::min(g.act, 4.0));
}

int AirbandHopper::tick(double clock, bool anyOpen, double centerHz) {
    if (grp_.size() < 2) return -1;
    if (waiting_) {
        if (clock - askedAt_ < 3.0) return -1;
        locate(centerHz, clock);   // the retune never came: where the radio really is
        return -1;
    }
    const double elapsed = clock - arrive_;
    if (anyOpen) lastOpen_ = clock;
    if (elapsed < kOpenMax && (anyOpen || clock - lastOpen_ < kOpenGrace)) return -1;
    if (elapsed < dwellSec()) return -1;
    G& me = grp_[cur_];
    me.dead = me.heard ? 0 : std::min(me.dead + 1, 6);
    me.act *= 0.9;
    me.lastVisit = clock;
    size_t best = cur_;
    double bp = -1;
    for (size_t i = 0; i < grp_.size(); i++) {
        if (i == cur_) continue;
        const G& g = grp_[i];
        const double age = std::min(clock - g.lastVisit, 600.0);
        double p = age * (1.0 + g.act) / (1.0 + g.dead);
        if (age >= kRevisitMax) p *= 100;
        if (p > bp) { bp = p; best = i; }
    }
    if (best == cur_) return -1;
    cur_ = best; waiting_ = true; askedAt_ = clock;
    return (int)best;
}

} // namespace dect2
