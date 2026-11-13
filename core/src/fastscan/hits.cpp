// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/fastscan/hits.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace fastscan {

std::vector<Candidate> findCandidates(const float* db, const float* fl, size_t n, double f0, double binHz, float onDb, float edgeDb, int bridge,
                                      const uint8_t* mask) {
    std::vector<Candidate> out;
    edgeDb = std::min(edgeDb, onDb);
    auto above = [&](size_t i, float lim) { return !(mask && mask[i]) && db[i] >= fl[i] + lim; };
    size_t i = 0;
    while (i < n) {
        if (!above(i, edgeDb)) { i++; continue; }
        size_t a = i, b = i, gap = 0;
        float best = -1e9f; size_t pk = i;
        for (size_t j = i; j < n; j++) {
            if (above(j, edgeDb)) {
                b = j; gap = 0;
                const float m = db[j] - fl[j];
                if (m > best) { best = m; pk = j; }
            } else if ((int)++gap > bridge) break;
        }
        i = b + 1;
        if (best < onDb) continue;
        double sw = 0, sf = 0;
        for (size_t j = a; j <= b; j++) {
            if (mask && mask[j]) continue;
            const double w = std::max(0.0, std::pow(10.0, db[j] / 10.0) - std::pow(10.0, fl[j] / 10.0));
            sw += w; sf += w * (double)j;
        }
        Candidate c;
        c.freqHz = f0 + (sw > 0 ? sf / sw : (double)pk) * binHz;
        c.bwHz = (double)(b - a + 1) * binHz;
        c.peakDb = db[pk]; c.snrDb = best;
        out.push_back(c);
    }
    return out;
}

bool HitTracker::isLocked(double hz) const { for (double f : locked_) if (std::fabs(f - hz) <= lockTol_) return true; return false; }
bool HitTracker::isPriority(double hz) const { for (double f : prio_) if (std::fabs(f - hz) <= prioTol_) return true; return false; }
int HitTracker::activeCount() const { int n = 0; for (const Hit& h : hits_) n += h.active; return n; }

int HitTracker::match(double hz, double bw) const {
    int best = -1;
    double bd = 1e30;
    for (size_t k = 0; k < hits_.size(); k++) {
        const double d = std::fabs(hits_[k].freqHz - hz);
        const double tol = p_.gridStepHz > 0 ? p_.gridStepHz * 0.5 : std::max(p_.mergeHz, 0.6 * std::max(bw, hits_[k].bwHz));
        if (d <= tol && d < bd) { bd = d; best = (int)k; }
    }
    return best;
}

void HitTracker::remove(uint64_t id) {
    for (size_t k = 0; k < hits_.size(); k++) if (hits_[k].id == id) { hits_.erase(hits_.begin() + k); st_.erase(st_.begin() + k); return; }
}

void HitTracker::trim() {
    while (hits_.size() > p_.maxHits) {
        int old = -1;
        for (size_t k = 0; k < hits_.size(); k++) if (!hits_[k].active && (old < 0 || hits_[k].lastSeen < hits_[old].lastSeen)) old = (int)k;
        if (old < 0) break;
        hits_.erase(hits_.begin() + old); st_.erase(st_.begin() + old);
    }
}

void HitTracker::observe(double t, double lo, double hi, const std::vector<Candidate>& cands, std::vector<HitEvent>& ev) {
    std::vector<char> seen(hits_.size(), 0);
    for (const Candidate& c : cands) {
        double f = c.freqHz;
        if (p_.gridStepHz > 0) f = p_.gridOriginHz + std::round((f - p_.gridOriginHz) / p_.gridStepHz) * p_.gridStepHz;
        if (inRanges(excl_, f) || isLocked(f)) continue;
        int k = match(f, c.bwHz);
        if (k < 0) {
            if (c.snrDb < p_.onDb) continue;
            Hit h;
            h.id = nextId_++; h.freqHz = f; h.bwHz = c.bwHz; h.peakDb = c.peakDb; h.snrDb = c.snrDb; h.maxSnrDb = c.snrDb;
            h.firstSeen = h.lastSeen = t; h.priority = isPriority(f);
            hits_.push_back(h); st_.push_back(St()); seen.push_back(0);
            k = (int)hits_.size() - 1;
        }
        seen[k] = 1;
        Hit& h = hits_[k];
        St& s = st_[k];
        h.peakDb = c.peakDb; h.snrDb = c.snrDb; h.maxSnrDb = std::max(h.maxSnrDb, c.snrDb);
        if (p_.gridStepHz <= 0 && c.snrDb >= p_.offDb) h.freqHz = f;   // a drifting carrier is followed
        if (!h.active) {
            if (c.snrDb >= p_.onDb) {
                if (!s.pending) { s.pending = true; s.pendingSince = t; }
                h.bwHz = c.bwHz;
                if (t - s.pendingSince >= p_.minDurSec) {
                    h.active = true; h.count++; h.startedAt = s.pendingSince; s.pending = false;
                    h.lastSeen = s.lastAbove = t; h.priority = isPriority(h.freqHz);
                    HitEvent e; e.kind = HitEvent::Start; e.hit = h; ev.push_back(e);
                } else s.lastAbove = t;
            } else s.pending = false;   // between off and on: not enough to start
        } else if (c.snrDb >= p_.offDb) {
            h.lastSeen = s.lastAbove = t;
            h.bwHz = std::max(h.bwHz * 0.9, c.bwHz);   // a wide signal does not flicker narrower
        }
    }
    for (size_t k = 0; k < hits_.size(); k++) {
        Hit& h = hits_[k];
        St& s = st_[k];
        const bool covered = h.freqHz >= lo && h.freqHz <= hi;
        const bool sawIt = k < seen.size() && seen[k];
        if (covered && !sawIt) s.pending = false;
        if (h.active && (covered || t - s.lastAbove >= p_.staleSec) && !(sawIt && h.snrDb >= p_.offDb) && t - s.lastAbove >= p_.hangSec) {
            h.active = false;
            h.activeSec += std::max(0.0, s.lastAbove - h.startedAt);
            HitEvent e; e.kind = HitEvent::End; e.hit = h; ev.push_back(e);
        }
    }
    // never-active hits that are gone leave no row
    for (size_t k = hits_.size(); k-- > 0;)
        if (!hits_[k].active && hits_[k].count == 0 && !st_[k].pending) { hits_.erase(hits_.begin() + k); st_.erase(st_.begin() + k); }
    trim();
}

void HitTracker::endAll(double t, std::vector<HitEvent>& ev) {
    (void)t;
    for (size_t k = 0; k < hits_.size(); k++) {
        Hit& h = hits_[k];
        if (!h.active) continue;
        h.active = false;
        h.activeSec += std::max(0.0, st_[k].lastAbove - h.startedAt);
        HitEvent e; e.kind = HitEvent::End; e.hit = h; ev.push_back(e);
    }
}

}  // namespace fastscan
}  // namespace dect2
