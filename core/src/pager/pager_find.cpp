// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/pager/pager_find.h"
#include "dect2/engine/wide_modes.h"
#include "dect2/t2/t2rx.h"
#include <algorithm>
#include <cmath>
#include <mutex>

namespace dect2 {

namespace {
constexpr double kMatchHz = 4000;   // a page belongs to the candidate whose frequency is this close
}

struct PagerFinder::Impl {
    mutable std::mutex mu;
    WideModes wm;
    double rate = 0, centre = 0, maxSec = 90, seen = 0, pollAt = 0;
    uint64_t seq = 0;
    std::vector<PagerCandidate> cand;
    RxTelemetry tel;

    static constexpr int kPager = 24;

    PagerCandidate* find(double hz) {
        PagerCandidate* best = nullptr;
        for (auto& c : cand) if (std::fabs(c.hz - hz) <= kMatchHz && (!best || std::fabs(c.hz - hz) < std::fabs(best->hz - hz))) best = &c;
        return best;
    }

    void poll() {
        if (!wm.telemetry(kPager, tel, seq)) return;
        for (const WideCarrier& w : tel.wide) {
            PagerCandidate* c = find(w.hz);
            if (!c) { cand.emplace_back(); c = &cand.back(); c->hz = w.hz; }
            c->snrDb = std::max(c->snrDb, w.snrDb);
            c->absHz = centre > 0 ? centre + c->hz : 0;
        }
        for (auto& c : cand) { c.pages = 0; for (auto& s : c.speedPages) s = 0; c.lastCapcode = 0; }
        if (tel.pager.messages) {
            uint64_t newest = 0;
            for (const PagerMessage& m : *tel.pager.messages) {
                if (m.selcall || m.speed < 0 || m.speed >= kPagerSpeeds) continue;
                PagerCandidate* c = find(m.carrierHz);
                if (!c) { cand.emplace_back(); c = &cand.back(); c->hz = m.carrierHz; c->absHz = centre > 0 ? centre + c->hz : 0; }
                c->pages++; c->speedPages[m.speed]++;
                if (m.wallTime >= (int64_t)newest) { newest = (uint64_t)m.wallTime; c->lastCapcode = m.address; }
            }
        }
        for (auto& c : cand) {
            int best = -1; uint64_t bn = 0;
            for (int i = 0; i < kPagerSpeeds; i++) if (c.speedPages[i] > bn) { bn = c.speedPages[i]; best = i; }
            c.speed = best;
        }
    }
};

PagerFinder::PagerFinder() : p_(new Impl) {}
PagerFinder::~PagerFinder() = default;

bool PagerFinder::configure(double rate, double centreHz, double maxSec) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!(rate >= 400e3)) return false;
    p_->rate = rate; p_->centre = centreHz; p_->maxSec = maxSec;
    // the 25 kHz raster is absolute: its channel centres are multiples of 25 kHz in frequency, not in the offset from the capture centre
    const double origin = centreHz > 0 ? -std::fmod(centreHz, 25000.0) : 0;
    p_->wm.configure(Impl::kPager, rate, origin);
    p_->wm.setPagerOptions(3, 7, false);
    p_->cand.clear(); p_->seen = 0; p_->pollAt = 0; p_->seq = 0;
    return true;
}

void PagerFinder::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->wm.reset(); p_->wm.clearResults();
    if (p_->rate > 0) p_->wm.configure(Impl::kPager, p_->rate, p_->centre > 0 ? -std::fmod(p_->centre, 25000.0) : 0);
    p_->cand.clear(); p_->seen = 0; p_->pollAt = 0; p_->seq = 0;
}

void PagerFinder::feed(const cf32* x, size_t n) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->rate <= 0 || p_->seen >= p_->maxSec) return;
    p_->wm.feed(Impl::kPager, x, n);
    p_->seen += (double)n / p_->rate;
    if (p_->seen >= p_->pollAt) { p_->pollAt = p_->seen + 0.5; p_->poll(); }
    if (p_->seen >= p_->maxSec) p_->poll();
}

std::vector<PagerCandidate> PagerFinder::rows() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto v = p_->cand;
    std::stable_sort(v.begin(), v.end(), [](const PagerCandidate& a, const PagerCandidate& b) {
        if ((a.pages > 0) != (b.pages > 0)) return a.pages > 0;
        if (a.pages != b.pages) return a.pages > b.pages;
        return a.snrDb > b.snrDb;
    });
    return v;
}

double PagerFinder::seconds() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->seen; }
bool PagerFinder::done() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate > 0 && p_->seen >= p_->maxSec; }
void PagerFinder::setLimit(double s) { std::lock_guard<std::mutex> lk(p_->mu); p_->maxSec = s; }

} // namespace dect2
