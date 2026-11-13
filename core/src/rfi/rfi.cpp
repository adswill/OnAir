// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Interference hunter: see include/dect2/rfi/rfi.h and docs/rfi-hunter.md. Original code, written from the general shape of the problem
// (spur picking against a running median, regular-grid fitting, rules of thumb for what typical interferers look like).
#include "dect2/rfi/rfi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <numeric>

namespace dect2 {
namespace rfi {

const char* kindName(Kind k) {
    switch (k) {
        case Kind::SwitchingSupply: return "Switching power supply";
        case Kind::LedLamp: return "LED lamp or driver";
        case Kind::Powerline: return "Powerline (PLC)";
        case Kind::LoLeak: return "Local oscillator or clock leak";
        default: return "Unknown";
    }
}

namespace {

float medianOf(std::vector<float>& v) {
    if (v.empty()) return 0;
    const size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + (long)m, v.end());
    return v[m];
}

// value at the given fraction (0.5 = median) over a centred window of w (odd) bins; the window shrinks at the edges
std::vector<float> runningQuantile(const std::vector<float>& v, int w, double frac) {
    const int n = (int)v.size(), h = w / 2;
    std::vector<float> out(v.size()), win;
    win.reserve((size_t)w);
    for (int i = 0; i < n; i++) {
        win.assign(v.begin() + std::max(0, i - h), v.begin() + std::min(n, i + h + 1));
        const size_t k = std::min(win.size() - 1, (size_t)(frac * (double)(win.size() - 1) + 0.5));
        std::nth_element(win.begin(), win.begin() + (long)k, win.end());
        out[(size_t)i] = win[k];
    }
    return out;
}

int oddBins(double widthHz, double binHz, int lo, int hi) {
    int w = (int)std::lround(widthHz / std::max(1.0, binHz));
    w = std::clamp(w, lo, hi);
    return w | 1;
}

struct HamBand { double lo, hi; };
const HamBand kHam[] = {{1.8e6, 2.0e6}, {3.5e6, 4.0e6}, {7.0e6, 7.3e6}, {10.1e6, 10.15e6}, {14.0e6, 14.35e6},
                        {18.068e6, 18.168e6}, {21.0e6, 21.45e6}, {24.89e6, 24.99e6}, {28.0e6, 29.7e6}};
bool overlapsHam(double lo, double hi) {
    for (const HamBand& b : kHam) if (lo < b.hi && hi > b.lo) return true;
    return false;
}

// common reference clocks (MHz) whose harmonics leak out of radios, SDRs and digital boards
const double kClocksMhz[] = {10, 12, 16, 19.2, 20, 24, 25, 26, 27, 30, 32, 38.4, 40, 48, 50, 100, 125};
double nearClock(double f, double binHz) {
    double best = 0, bestRes = 1e18;
    for (double cm : kClocksMhz) {
        const double c = cm * 1e6;
        if (f < c - 1) continue;
        const double k = std::round(f / c);
        if (k < 1 || k > 60) continue;
        const double res = std::fabs(f - k * c);
        const double tol = std::max(1.5 * binHz, 20e-6 * f);
        if (res <= tol && res < bestRes) { bestRes = res; best = c; }
    }
    return best;
}

} // namespace

std::vector<Spur> findSpurs(const Sweep& s, const SpurConfig& c) {
    std::vector<Spur> out;
    const size_t n = s.bins();
    if (n < 5 || s.binHz <= 0) return out;
    const int w = oddBins(c.localWindowHz, s.binHz, 9, 201);
    const std::vector<float> loc = runningQuantile(s.db, w, 0.2);   // low quantile: a comb with a tooth every few bins must not lift its own reference
    std::vector<char> up(n, 0);
    for (size_t i = 0; i < n; i++) {
        const float fl = i < s.floorDb.size() ? s.floorDb[i] : s.floorOverallDb;
        up[i] = s.db[i] >= loc[i] + (float)c.thresholdDb && s.db[i] >= fl + (float)c.thresholdDb;
    }
    size_t i = 0;
    while (i < n) {
        if (!up[i]) { i++; continue; }
        size_t a = i, b = i;
        while (b + 2 < n && (up[b + 1] || up[b + 2])) b++;   // bridge a gap of one bin
        while (b > a && !up[b]) b--;
        i = b + 1;
        // one spur per peak: peaks closer than 3 dB above the valley between them are one spur (a ripple on a flat top)
        std::vector<size_t> peaks;
        for (size_t j = a; j <= b; j++) {
            const bool l = j == a || s.db[j] > s.db[j - 1], r = j == b || s.db[j] >= s.db[j + 1];
            if (l && r) peaks.push_back(j);
        }
        std::vector<size_t> kept;
        for (size_t q : peaks) {
            if (kept.empty()) { kept.push_back(q); continue; }
            const size_t p0 = kept.back();
            float valley = s.db[p0];
            for (size_t j = p0; j <= q; j++) valley = std::min(valley, s.db[j]);
            if (valley < std::min(s.db[p0], s.db[q]) - 3.f) kept.push_back(q);
            else if (s.db[q] > s.db[p0]) kept.back() = q;
        }
        if ((double)(b - a + 1) * s.binHz > c.maxBwHz && kept.size() < 3) continue;   // a wide signal, not a spur
        for (size_t ki = 0; ki < kept.size(); ki++) {
            const size_t pk = kept[ki];
            size_t lo = pk, hi = pk;   // the extent of this peak: down to the dip towards the neighbours or the end of the run
            const float edge = s.db[pk] - 10.f;   // the width is measured 10 dB down from the peak, so sidelobes do not widen a tone
            while (lo > a && up[lo - 1] && s.db[lo - 1] >= edge && s.db[lo - 1] <= s.db[lo] && (ki == 0 || lo - 1 > kept[ki - 1])) lo--;
            while (hi < b && up[hi + 1] && s.db[hi + 1] >= edge && s.db[hi + 1] <= s.db[hi] && (ki + 1 == kept.size() || hi + 1 < kept[ki + 1])) hi++;
            const double width = (double)(hi - lo + 1) * s.binHz;
            if (width > c.maxBwHz) continue;
            double off = 0;
            if (pk > 0 && pk + 1 < n) {   // parabolic interpolation on the dB values
                const double y0 = s.db[pk - 1], y1 = s.db[pk], y2 = s.db[pk + 1], den = y0 - 2 * y1 + y2;
                if (den < -1e-6) off = std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5);
            }
            Spur sp;
            sp.freqHz = s.freqOf(pk) + off * s.binHz;
            sp.bwHz = std::max(width, s.binHz);
            sp.levelDb = s.db[pk] - loc[pk];
            out.push_back(sp);
        }
    }
    // a weak peak within four bins of a much stronger one is its sidelobe
    std::vector<Spur> clean;
    for (const Spur& x : out) {
        bool side = false;
        for (const Spur& y : out) if (&x != &y && std::fabs(x.freqHz - y.freqHz) <= 4 * s.binHz && y.levelDb >= x.levelDb + 8) { side = true; break; }
        if (!side) clean.push_back(x);
    }
    return clean;
}

std::vector<Hump> findHumps(const Sweep& s, const std::vector<float>& ref, double minRiseDb, double minWidthHz) {
    std::vector<Hump> out;
    const size_t n = s.bins();
    if (n < 9 || s.binHz <= 0) return out;
    const bool useRef = ref.size() == n;
    std::vector<float> d(n);
    for (size_t i = 0; i < n; i++) d[i] = s.db[i] - (useRef ? ref[i] : s.floorOverallDb);
    const int w = oddBins(minWidthHz / 2, s.binHz, 5, 201);
    const std::vector<float> sm = runningQuantile(d, w, 0.3);   // a low quantile over half the minimum width: a comb of spurs over a quiet floor is not a hump
    struct Run { size_t a, b; };
    std::vector<Run> runs;
    for (size_t i = 0; i < n;) {
        if (sm[i] < (float)minRiseDb) { i++; continue; }
        size_t j = i;
        while (j + 1 < n && sm[j + 1] >= (float)minRiseDb) j++;
        runs.push_back({i, j});
        i = j + 1;
    }
    // runs separated by a short gap are one hump with a notch (amateur bands are left out of powerline adapters)
    const size_t gapMax = (size_t)std::max(1.0, 1.0e6 / s.binHz);
    size_t k = 0;
    while (k < runs.size()) {
        size_t a = runs[k].a, b = runs[k].b;
        int notches = 0, ham = 0;
        size_t m = k + 1;
        while (m < runs.size() && runs[m].a - b <= gapMax) {
            notches++;
            if (overlapsHam(s.freqOf(b + 1), s.freqOf(runs[m].a - 1))) ham++;
            b = runs[m].b;
            m++;
        }
        k = m;
        const double width = (double)(b - a + 1) * s.binHz;
        if (width < 0.5 * minWidthHz) continue;   // the smoothing eats about half a window at each edge
        std::vector<float> v(d.begin() + (long)a, d.begin() + (long)b + 1);
        // a hump is smooth: most bins sit near its median level (a comb of spurs over a quiet floor is not a hump)
        const float med = medianOf(v);
        size_t nearBin = 0;
        for (float x : v) if (std::fabs(x - med) <= 5.f) nearBin++;
        if ((double)nearBin < 0.6 * (double)v.size()) continue;
        Hump h;
        h.startHz = s.freqOf(a); h.stopHz = s.freqOf(b); h.riseDb = med; h.notches = notches; h.hamNotches = ham;
        out.push_back(h);
    }
    return out;
}

Comb findComb(const std::vector<Spur>& spursIn, double binHz) {
    std::vector<size_t> idx(spursIn.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](size_t x, size_t y) { return spursIn[x].freqHz < spursIn[y].freqHz; });
    Comb none;
    if (idx.size() < 4) return none;
    auto F = [&](size_t i) { return spursIn[idx[i]].freqHz; };
    const size_t m = std::min<size_t>(idx.size(), 80);
    const double minS = std::max(5 * binHz, 5e3);
    struct Fit { double s = 0, anchor = 0; std::vector<size_t> pos; std::vector<long> k; };
    // the biggest run of teeth (no gap of more than one missing tooth) among the spurs that fall on the grid anchor + k * s
    auto evaluate = [&](double anchor, double s) {
        Fit f; f.s = s; f.anchor = anchor;
        const double tol = std::min(0.2 * s, std::max(1.0 * binHz, 0.004 * s));
        std::vector<std::pair<long, size_t>> hit;
        for (size_t p = 0; p < idx.size(); p++) {
            const long kk = std::lround((F(p) - anchor) / s);
            if (std::fabs(F(p) - (anchor + (double)kk * s)) <= tol) hit.push_back({kk, p});
        }
        std::sort(hit.begin(), hit.end());
        size_t bestA = 0, bestN = 0;
        for (size_t a = 0; a < hit.size();) {
            size_t b = a;
            while (b + 1 < hit.size() && hit[b + 1].first - hit[b].first <= 2) b++;
            if (b - a + 1 > bestN) { bestN = b - a + 1; bestA = a; }
            a = b + 1;
        }
        for (size_t q = bestA; q < bestA + bestN; q++) { f.pos.push_back(hit[q].second); f.k.push_back(hit[q].first); }
        return f;
    };
    Fit best;
    for (size_t i = 0; i < m; i++)
        for (size_t j = i + 1; j < std::min(m, i + 4); j++)
            for (int div = 1; div <= 4; div++) {
                const double s = (F(j) - F(i)) / div;
                if (s < minS || s > 5e6) continue;
                Fit f = evaluate(F(i), s);
                if (f.pos.size() < 4) continue;
                const long expected = f.k.back() - f.k.front() + 1;
                if ((double)f.pos.size() < 0.6 * (double)expected) continue;
                if (f.pos.size() > best.pos.size() || (f.pos.size() == best.pos.size() && s > best.s)) best = std::move(f);
            }
    if (best.pos.size() < 4) return none;
    // least-squares refinement of spacing and offset over the members
    Comb c;
    const long kmin = best.k.front();
    double sk = 0, sf = 0, skk = 0, skf = 0;
    const double N = (double)best.pos.size();
    for (size_t q = 0; q < best.pos.size(); q++) {
        const double k = (double)(best.k[q] - kmin), fr = F(best.pos[q]);
        sk += k; sf += fr; skk += k * k; skf += k * fr;
        c.members.push_back((int)idx[best.pos[q]]);
    }
    const double den = N * skk - sk * sk;
    c.spacingHz = den > 1e-9 ? (N * skf - sk * sf) / den : best.s;
    c.startHz = (sf - c.spacingHz * sk) / N;
    c.hits = (int)best.pos.size();
    c.expected = (int)(best.k.back() - best.k.front() + 1);
    return c;
}

std::vector<Finding> classify(const std::vector<Spur>& spurs, const std::vector<Hump>& humps, double binHz) {
    std::vector<Finding> out;
    std::vector<char> used(spurs.size(), 0);
    auto cap = [](float v) { return std::min(0.95f, v); };
    // 1. wide humps: LED lamps, powerline adapters
    for (const Hump& h : humps) {
        std::vector<size_t> inside;
        std::vector<Spur> sub;
        for (size_t i = 0; i < spurs.size(); i++) if (!used[i] && spurs[i].freqHz >= h.startHz && spurs[i].freqHz <= h.stopHz) { inside.push_back(i); sub.push_back(spurs[i]); }
        const Comb cb = findComb(sub, binHz);
        const double width = h.stopHz - h.startHz;
        Finding f;
        f.startHz = h.startHz; f.stopHz = h.stopHz; f.levelDb = h.riseDb; f.hamNotches = h.hamNotches;
        if (h.stopHz < 500e3 && width <= 200e3) { f.kind = Kind::Powerline; f.confidence = 0.6f; }
        else if (width >= 1e6 && h.startHz >= 1.6e6 && h.startHz < 30e6) {
            if (h.hamNotches >= 1) { f.kind = Kind::Powerline; f.confidence = cap(0.5f + 0.1f * (float)std::min(h.hamNotches, 4)); }
            else { f.kind = Kind::LedLamp; f.confidence = cb.ok() ? 0.55f : 0.35f; }
        } else {
            f.kind = Kind::LedLamp;
            f.confidence = 0.5f + (cb.ok() ? (cb.spacingHz >= 20e3 && cb.spacingHz <= 300e3 ? 0.25f : 0.1f) : 0.f) + (h.riseDb >= 8 ? 0.05f : 0.f);
            f.confidence = cap(f.confidence);
        }
        if (cb.ok()) {
            f.spacingHz = cb.spacingHz; f.count = cb.hits;
            for (int m : cb.members) { used[inside[(size_t)m]] = 1; f.members.push_back(sub[(size_t)m].freqHz); }
        }
        out.push_back(f);
    }
    // 2. combs of narrow spurs: switching supplies
    for (int round = 0; round < 4; round++) {
        std::vector<size_t> rest;
        std::vector<Spur> sub;
        for (size_t i = 0; i < spurs.size(); i++) if (!used[i]) { rest.push_back(i); sub.push_back(spurs[i]); }
        const Comb cb = findComb(sub, binHz);
        if (!cb.ok()) break;
        Finding f;
        f.kind = Kind::SwitchingSupply; f.spacingHz = cb.spacingHz; f.count = cb.hits;
        f.confidence = cap(0.5f + 0.08f * (float)std::min(cb.hits - 4, 5) + (cb.spacingHz >= 20e3 && cb.spacingHz <= 1e6 ? 0.05f : 0.f));
        f.startHz = 1e30; f.stopHz = 0;
        for (int m : cb.members) {
            const Spur& sp = sub[(size_t)m];
            used[rest[(size_t)m]] = 1; f.members.push_back(sp.freqHz);
            f.startHz = std::min(f.startHz, sp.freqHz); f.stopHz = std::max(f.stopHz, sp.freqHz); f.levelDb = std::max(f.levelDb, sp.levelDb);
        }
        out.push_back(f);
    }
    // 3. lone spurs: narrowband powerline carriers below 500 kHz, clock and oscillator leaks
    for (size_t i = 0; i < spurs.size(); i++) {
        if (used[i]) continue;
        const Spur& sp = spurs[i];
        Finding f;
        f.startHz = f.stopHz = sp.freqHz; f.levelDb = sp.levelDb; f.members.push_back(sp.freqHz); f.count = 1;
        if (sp.freqHz < 500e3) { f.kind = Kind::Powerline; f.confidence = sp.bwHz >= 4e3 ? 0.55f : 0.4f; }
        else if (sp.levelDb >= 12 && sp.bwHz <= 3 * binHz + 1) {
            f.kind = Kind::LoLeak; f.clockHz = nearClock(sp.freqHz, binHz);
            f.confidence = 0.4f + (f.clockHz > 0 ? 0.25f : 0.f) + (sp.levelDb >= 25 ? 0.1f : 0.f);
        } else continue;
        out.push_back(f);
    }
    return out;
}

// ---- monitor ----

bool Monitor::sameGrid(const Sweep& s) const {
    const Sweep& r = ready_ ? baseSweep_ : (learn_.empty() ? s : learn_.front());
    return r.bins() == s.bins() && std::fabs(r.startHz - s.startHz) < 0.5 * s.binHz && std::fabs(r.binHz - s.binHz) < 1e-6 * s.binHz + 1e-9;
}

void Monitor::relearn() {
    ready_ = false; learn_.clear(); cands_.clear(); active_.clear(); base_.clear(); baseSpurs_.clear();
    floorHits_ = floorClear_ = 0; floorAlerted_ = false;
}

void Monitor::accept() {
    if (!ready_ || last_.bins() != base_.size()) return;
    for (const Cand& c : cands_) {
        if (!c.alerted) continue;
        if (c.type == 0) { Spur sp; sp.freqHz = c.freqHz; sp.bwHz = c.bwHz; sp.levelDb = c.levelDb; baseSpurs_.push_back(sp); }
        else for (size_t i = 0; i < base_.size(); i++) { const double f = last_.freqOf(i); if (f >= c.freqHz && f <= c.freqHz + c.bwHz) base_[i] = last_.db[i]; }
    }
    if (floorAlerted_) floorBase_ = last_.floorOverallDb;
    floorAlerted_ = false; floorHits_ = floorClear_ = 0;
    cands_.clear(); active_.clear();
    baseSweep_.db = base_;
}

std::vector<Alert> Monitor::add(const Sweep& s) {
    std::vector<Alert> alerts;
    if (s.bins() < 9 || s.binHz <= 0) return alerts;
    if (!sameGrid(s)) relearn();
    last_ = s;
    FloorPoint fp; fp.time = s.timeSec; fp.floorDb = s.floorOverallDb;
    if (!ready_) {
        learn_.push_back(s);
        hist_.push_back(fp);
        if ((int)learn_.size() >= std::max(1, cfg_.learnSweeps)) {
            const size_t n = s.bins();
            base_.assign(n, 0.f);
            std::vector<float> col(learn_.size());
            for (size_t i = 0; i < n; i++) {
                for (size_t k = 0; k < learn_.size(); k++) col[k] = learn_[k].db[i];
                base_[i] = medianOf(col);
            }
            std::vector<float> fl;
            for (const Sweep& l : learn_) fl.push_back(l.floorOverallDb);
            floorBase_ = medianOf(fl);
            baseSweep_ = s; baseSweep_.db = base_;
            baseSweep_.floorDb.assign(n, floorBase_); baseSweep_.floorOverallDb = floorBase_;
            baseSpurs_ = findSpurs(baseSweep_, cfg_.spur);
            ready_ = true; learn_.clear();
        }
        while (hist_.size() > cfg_.maxHistory) hist_.pop_front();
        return alerts;
    }
    const double delta = s.floorOverallDb - floorBase_;
    fp.deltaDb = (float)delta;
    hist_.push_back(fp);
    while (hist_.size() > cfg_.maxHistory) hist_.pop_front();

    const double binHz = s.binHz;
    const double tol = 2 * binHz;
    // spurs that are not in the baseline (or that grew a lot)
    std::vector<Spur> fresh;
    for (const Spur& sp : findSpurs(s, cfg_.spur)) {
        bool known = false;
        for (const Spur& b : baseSpurs_) if (std::fabs(b.freqHz - sp.freqHz) <= tol && sp.levelDb < b.levelDb + 8) { known = true; break; }
        if (!known) fresh.push_back(sp);
    }
    const std::vector<Hump> humps = findHumps(s, base_, cfg_.humpRiseDb, cfg_.humpMinWidthHz);
    std::vector<char> seen(cands_.size(), 0);
    for (const Spur& sp : fresh) {
        int hit = -1;
        for (size_t i = 0; i < cands_.size(); i++) if (cands_[i].type == 0 && !seen[i] && std::fabs(cands_[i].freqHz - sp.freqHz) <= tol) { hit = (int)i; break; }
        if (hit >= 0) {
            Cand& c = cands_[(size_t)hit]; seen[(size_t)hit] = 1;
            c.hits++; c.miss = 0; c.freqHz = sp.freqHz; c.bwHz = sp.bwHz; c.levelDb = sp.levelDb;
        } else {
            Cand c; c.type = 0; c.freqHz = sp.freqHz; c.bwHz = sp.bwHz; c.levelDb = sp.levelDb; c.hits = 1;
            cands_.push_back(c); seen.push_back(1);
        }
    }
    for (const Hump& h : humps) {
        int hit = -1;
        for (size_t i = 0; i < cands_.size(); i++) {
            if (cands_[i].type != 1 || seen[i]) continue;
            const double lo = std::max(cands_[i].freqHz, h.startHz), hi = std::min(cands_[i].freqHz + cands_[i].bwHz, h.stopHz);
            if (hi - lo >= 0.5 * std::min(cands_[i].bwHz, h.stopHz - h.startHz)) { hit = (int)i; break; }
        }
        Cand* c;
        if (hit >= 0) { c = &cands_[(size_t)hit]; seen[(size_t)hit] = 1; c->hits++; c->miss = 0; }
        else { cands_.push_back(Cand()); seen.push_back(1); c = &cands_.back(); c->type = 1; c->hits = 1; }
        c->freqHz = h.startHz; c->bwHz = h.stopHz - h.startHz; c->levelDb = h.riseDb; c->ham = h.hamNotches; c->notch = h.notches;
    }
    // unseen candidates: dropped (never confirmed) or cleared (confirmed, gone for clearSweeps)
    std::vector<Cand> keep;
    for (size_t i = 0; i < cands_.size(); i++) {
        Cand c = cands_[i];
        if (i < seen.size() && !seen[i]) {
            c.miss++;
            if (!c.alerted) continue;
            if (c.miss >= cfg_.clearSweeps) {
                Alert a; a.type = Alert::Cleared; a.time = s.timeSec; a.freqHz = c.freqHz; a.bwHz = c.bwHz; a.levelDb = c.levelDb; a.kind = c.kind; a.confidence = c.conf; a.spacingHz = c.spacing; a.count = c.count;
                alerts.push_back(a);
                continue;
            }
        }
        keep.push_back(c);
    }
    cands_ = keep;
    // classify everything confirmed, then raise the alerts for the candidates that have just been confirmed
    std::vector<Spur> cs;
    std::vector<Hump> ch;
    bool newlyConfirmed = false;
    for (const Cand& c : cands_) {
        if (c.hits < cfg_.confirmSweeps) continue;
        if (!c.alerted) newlyConfirmed = true;
        if (c.type == 0) { Spur sp; sp.freqHz = c.freqHz; sp.bwHz = c.bwHz; sp.levelDb = c.levelDb; cs.push_back(sp); }
        else { Hump h; h.startHz = c.freqHz; h.stopHz = c.freqHz + c.bwHz; h.riseDb = c.levelDb; h.hamNotches = c.ham; h.notches = c.notch; ch.push_back(h); }
    }
    if (newlyConfirmed || (cs.size() + ch.size()) != active_.size()) active_ = classify(cs, ch, binHz);
    for (Cand& c : cands_) {
        if (c.hits < cfg_.confirmSweeps) continue;
        const Finding* fnd = nullptr;
        for (const Finding& f : active_) {
            bool in = false;
            if (c.type == 0) for (double m : f.members) if (std::fabs(m - c.freqHz) <= tol) in = true;
            if (c.type == 1 && f.kind != Kind::SwitchingSupply && f.kind != Kind::LoLeak && f.startHz < c.freqHz + c.bwHz && f.stopHz > c.freqHz) in = true;
            if (in && (!fnd || f.confidence > fnd->confidence)) fnd = &f;
        }
        if (fnd) { c.kind = fnd->kind; c.conf = fnd->confidence; c.spacing = fnd->spacingHz; c.count = fnd->count; }
        if (c.alerted) continue;
        c.alerted = true;
        Alert a; a.type = c.type == 0 ? Alert::NewSpur : Alert::NewHump; a.time = s.timeSec; a.freqHz = c.freqHz; a.bwHz = c.bwHz; a.levelDb = c.levelDb;
        a.kind = c.kind; a.confidence = c.conf; a.spacingHz = c.spacing; a.count = c.count;
        alerts.push_back(a);
    }
    // the noise floor
    if (delta >= cfg_.floorRiseDb) {
        floorHits_++; floorClear_ = 0;
        if (floorHits_ >= cfg_.confirmSweeps && !floorAlerted_) {
            floorAlerted_ = true;
            Alert a; a.type = Alert::FloorRise; a.time = s.timeSec; a.levelDb = (float)delta;
            for (const Finding& f : active_) if (f.kind == Kind::LedLamp || f.kind == Kind::Powerline) { a.kind = f.kind; a.confidence = f.confidence; a.freqHz = f.startHz; a.bwHz = f.stopHz - f.startHz; break; }
            alerts.push_back(a);
        }
    } else {
        floorHits_ = 0;
        if (floorAlerted_ && delta < cfg_.floorRiseDb / 2) {
            if (++floorClear_ >= cfg_.clearSweeps) {
                floorAlerted_ = false; floorClear_ = 0;
                Alert a; a.type = Alert::Cleared; a.time = s.timeSec; a.levelDb = (float)delta; alerts.push_back(a);
            }
        }
        if (!floorAlerted_) floorBase_ += (float)(0.02 * delta);   // the baseline floor follows slow drift (temperature, gain), not interference
    }
    for (const Alert& a : alerts) log_.push_back(a);
    return alerts;
}

static const char* alertTypeName(Alert::Type t) {
    switch (t) { case Alert::NewSpur: return "new spur"; case Alert::FloorRise: return "floor rise"; case Alert::NewHump: return "new broadband"; default: return "cleared"; }
}

bool Monitor::exportLogCsv(const std::string& path, std::string& err) const {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { err = "cannot write " + path; return false; }
    fprintf(f, "unix_time,event,freq_hz,bandwidth_hz,level_db,source,confidence,comb_spacing_hz,comb_teeth\n");
    for (const Alert& a : log_)
        fprintf(f, "%.0f,%s,%.0f,%.0f,%.1f,%s,%.2f,%.0f,%d\n", a.time, alertTypeName(a.type), a.freqHz, a.bwHz, a.levelDb, kindName(a.kind), a.confidence, a.spacingHz, a.count);
    fclose(f);
    return true;
}

bool Monitor::exportFloorCsv(const std::string& path, std::string& err) const {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { err = "cannot write " + path; return false; }
    fprintf(f, "unix_time,floor_dbfs,change_vs_baseline_db\n");
    for (const FloorPoint& p : hist_) fprintf(f, "%.0f,%.2f,%.2f\n", p.time, p.floorDb, p.deltaDb);
    fclose(f);
    return true;
}

// ---- walk ----

const Reading& Walk::add(const Sweep& s, const double* lat, const double* lon) {
    last_ = s;
    Reading r;
    r.time = s.timeSec; r.floorDb = s.floorOverallDb;
    if (lat && lon) { r.hasPos = true; r.lat = *lat; r.lon = *lon; }
    double p = 0;
    size_t cnt = 0;
    for (size_t i = 0; i < s.bins(); i++) {
        const double f = s.freqOf(i);
        bool skip = false;
        for (const Spur& sp : ignore_) if (std::fabs(f - sp.freqHz) <= std::max(sp.bwHz, 2 * s.binHz)) { skip = true; break; }
        if (skip) continue;
        p += std::pow(10.0, s.db[i] / 10.0); cnt++;
    }
    r.powerDb = cnt ? (float)(10.0 * std::log10(std::max(1e-30, p / (double)cnt))) : -150.f;
    if (!have_) { smoothed_ = r.powerDb; minSmoothed_ = r.powerDb; have_ = true; }
    else {
        const double a = r.powerDb > smoothed_ ? cfg_.attack : cfg_.decay;
        smoothed_ += (float)(a * (r.powerDb - smoothed_));
        minSmoothed_ = std::min(minSmoothed_, smoothed_);
    }
    r.levelDb = r.powerDb - referenceDb();
    meter_ = (float)std::clamp((smoothed_ - referenceDb()) / std::max(1.0, cfg_.rangeDb), 0.0, 1.0);
    const std::vector<Spur> sp = findSpurs(s, cfg_.spur);
    for (const Spur& x : sp) {
        bool ign = false;
        for (const Spur& g : ignore_) if (std::fabs(g.freqHz - x.freqHz) <= std::max(g.bwHz, 2 * s.binHz)) { ign = true; break; }
        if (!ign && x.levelDb > r.peakDb) { r.peakDb = x.levelDb; r.peakHz = x.freqHz; }
    }
    std::vector<Spur> use;
    for (const Spur& x : sp) {
        bool ign = false;
        for (const Spur& g : ignore_) if (std::fabs(g.freqHz - x.freqHz) <= std::max(g.bwHz, 2 * s.binHz)) { ign = true; break; }
        if (!ign) use.push_back(x);
    }
    float bc = 0;
    for (const Finding& f : classify(use, findHumps(s, std::vector<float>(), 4, 400e3), s.binHz)) if (f.confidence > bc) { bc = f.confidence; r.kind = f.kind; }
    recent_.push_back(r.powerDb);
    if (recent_.size() > 6) recent_.erase(recent_.begin());
    rd_.push_back(r);
    return rd_.back();
}

void Walk::setReferenceNow() { if (!have_) return; ref_ = smoothed_; manualRef_ = true; }

void Walk::ignoreNow() {
    ignore_.clear();
    for (const Spur& sp : findSpurs(last_, cfg_.spur)) ignore_.push_back(sp);
    recent_.clear();
    have_ = false;   // the power changes by what was left out: smooth and find the quietest point again
    manualRef_ = false;
}

void Walk::clear() { rd_.clear(); recent_.clear(); have_ = false; manualRef_ = false; meter_ = 0; smoothed_ = minSmoothed_ = 0; }

int Walk::trend() const {
    if (recent_.size() < 6) return 0;
    const float a = (recent_[3] + recent_[4] + recent_[5]) / 3, b = (recent_[0] + recent_[1] + recent_[2]) / 3;
    return a - b > 1.f ? 1 : (a - b < -1.f ? -1 : 0);
}

int Walk::loudest() const {
    int best = -1;
    for (size_t i = 0; i < rd_.size(); i++) if (best < 0 || rd_[i].powerDb > rd_[(size_t)best].powerDb) best = (int)i;
    return best;
}

static std::string isoTime(double t) {
    time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[40];
    strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &g);
    return b;
}

bool Walk::exportCsv(const std::string& path, std::string& err) const {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { err = "cannot write " + path; return false; }
    fprintf(f, "time_utc,unix_time,latitude,longitude,power_dbfs,level_db,floor_dbfs,strongest_spur_hz,strongest_spur_db,source\n");
    for (const Reading& r : rd_) {
        if (r.hasPos) fprintf(f, "%s,%.0f,%.6f,%.6f,%.2f,%.2f,%.2f,%.0f,%.1f,%s\n", isoTime(r.time).c_str(), r.time, r.lat, r.lon, r.powerDb, r.levelDb, r.floorDb, r.peakHz, r.peakDb, kindName(r.kind));
        else fprintf(f, "%s,%.0f,,,%.2f,%.2f,%.2f,%.0f,%.1f,%s\n", isoTime(r.time).c_str(), r.time, r.powerDb, r.levelDb, r.floorDb, r.peakHz, r.peakDb, kindName(r.kind));
    }
    fclose(f);
    return true;
}

bool Walk::exportGpx(const std::string& path, std::string& err) const {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { err = "cannot write " + path; return false; }
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<gpx version=\"1.1\" creator=\"OnAir interference hunter\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n");
    for (const Reading& r : rd_) {
        if (!r.hasPos) continue;
        fprintf(f, "<wpt lat=\"%.6f\" lon=\"%.6f\"><time>%s</time><name>%+.1f dB</name><desc>%s, strongest spur %.4f MHz</desc></wpt>\n",
                r.lat, r.lon, isoTime(r.time).c_str(), r.levelDb, kindName(r.kind), r.peakHz / 1e6);
    }
    fprintf(f, "</gpx>\n");
    fclose(f);
    return true;
}

// ---- NMEA ----

static double nmeaAngle(const std::string& v, const std::string& hemi, int degDigits) {
    if (v.size() < (size_t)degDigits + 2 || hemi.empty()) return 1e9;
    const double deg = atof(v.substr(0, (size_t)degDigits).c_str()), min = atof(v.substr((size_t)degDigits).c_str());
    double r = deg + min / 60.0;
    if (hemi[0] == 'S' || hemi[0] == 'W') r = -r;
    return r;
}

bool parseNmea(const std::string& lineIn, Fix& fix) {
    std::string line = lineIn;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
    if (line.size() < 8 || line[0] != '$') return false;
    const size_t star = line.find('*');
    std::string body = line.substr(1, star == std::string::npos ? std::string::npos : star - 1);
    if (star != std::string::npos) {
        unsigned x = 0;
        for (char c : body) x ^= (unsigned char)c;
        unsigned want = 0;
        if (line.size() < star + 3 || sscanf(line.c_str() + star + 1, "%2x", &want) != 1 || want != x) return false;
    }
    std::vector<std::string> f;
    size_t p = 0;
    for (;;) { const size_t q = body.find(',', p); f.push_back(body.substr(p, q == std::string::npos ? std::string::npos : q - p)); if (q == std::string::npos) break; p = q + 1; }
    if (f[0].size() < 5) return false;
    const std::string type = f[0].substr(f[0].size() - 3);
    auto utc = [&](const std::string& t) { return t.size() >= 6 ? atoi(t.substr(0, 2).c_str()) * 3600.0 + atoi(t.substr(2, 2).c_str()) * 60.0 + atof(t.substr(4).c_str()) : -1.0; };
    if (type == "GGA" && f.size() >= 8) {
        const double la = nmeaAngle(f[2], f[3], 2), lo = nmeaAngle(f[4], f[5], 3);
        fix.utcSec = utc(f[1]); fix.sats = atoi(f[7].c_str());
        fix.valid = atoi(f[6].c_str()) > 0 && la < 1e8 && lo < 1e8;
        if (fix.valid) { fix.lat = la; fix.lon = lo; }
        return true;
    }
    if (type == "RMC" && f.size() >= 7) {
        const double la = nmeaAngle(f[3], f[4], 2), lo = nmeaAngle(f[5], f[6], 3);
        fix.utcSec = utc(f[1]);
        fix.valid = f[2] == "A" && la < 1e8 && lo < 1e8;
        if (fix.valid) { fix.lat = la; fix.lon = lo; }
        return true;
    }
    return false;
}

// ---- simulation ----

namespace {
struct Rng {
    uint64_t s;
    uint64_t next() { s += 0x9E3779B97F4A7C15ull; uint64_t z = s; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
    double uni() { return (double)(next() >> 11) / 9007199254740992.0; }
    double gauss() { return (uni() + uni() + uni() + uni() - 2.0) * 1.732; }   // sd 1
};
void addPower(float& db, double levelDb) { db = (float)(10.0 * std::log10(std::pow(10.0, db / 10.0) + std::pow(10.0, levelDb / 10.0))); }
}

Sweep simulateSweep(const SimScene& sc, double t) {
    Sweep s;
    s.startHz = sc.startHz; s.binHz = sc.binHz; s.timeSec = t; s.rateHz = 10e6; s.averages = 16; s.hops = 1;
    const size_t n = (size_t)std::lround((sc.stopHz - sc.startHz) / sc.binHz) + 1;
    Rng g{sc.seed ^ (uint64_t)(t * 1000.0) * 0x2545F4914F6CDD1Dull};
    s.db.assign(n, sc.floorDb);
    for (size_t i = 0; i < n; i++) s.db[i] += (float)(0.6 * g.gauss());
    auto bin = [&](double f) { return (f - sc.startHz) / sc.binHz; };
    for (const SimSource& src : sc.sources) {
        if (t < src.onSec || t >= src.offSec) continue;
        const float base = sc.floorDb;
        auto tone = [&](double f, double lvl) {   // a tone shared between the two nearest bins by its position, with skirts
            const double b = bin(f);
            const long i0 = (long)std::floor(b);
            const double fr = b - (double)i0;
            auto put = [&](long i, double w, double extra) { if (i >= 0 && i < (long)n && w > 1e-3) addPower(s.db[(size_t)i], base + lvl + extra + 10.0 * std::log10(w)); };
            put(i0, 1.0 - fr, 0); put(i0 + 1, fr, 0);
            put(i0 - 1, 1.0, -18); put(i0 + 2, 1.0, -18);
        };
        auto block = [&](double f0, double f1, double lvl, bool hamNotch) {
            for (size_t i = 0; i < n; i++) {
                const double f = sc.startHz + (double)i * sc.binHz;
                if (f < f0 || f > f1) continue;
                if (hamNotch && overlapsHam(f - sc.binHz / 2, f + sc.binHz / 2)) continue;
                addPower(s.db[i], base + lvl + 0.8 * g.gauss());
            }
        };
        switch (src.type) {
            case SimSource::Carrier: case SimSource::Lo: tone(src.freqHz, src.levelDb); break;
            case SimSource::Comb:
                for (int k = 0; k < src.teeth; k++) tone(src.freqHz + k * src.spacingHz, src.levelDb - 0.5 * k);
                break;
            case SimSource::Hump: block(src.freqHz, src.freqHz + src.widthHz, src.levelDb, false); break;
            case SimSource::PlcHf: block(src.freqHz, src.freqHz + src.widthHz, src.levelDb, true); break;
        }
    }
    s.floorOverallDb = sweep::noiseFloorDb(s.db.data(), n, 50, s.averages);
    s.floorDb.assign(n, s.floorOverallDb);
    return s;
}

} // namespace rfi
} // namespace dect2
