// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AM modulation health (see am_health.h).
#include "dect2/smart5/am_health.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace smart5 {

std::string AmHealthReport::summary() const {
    std::string s; char b[96];
    auto add = [&](const char* x) { if (!s.empty()) s += "; "; s += x; };
    if (overModulated) { snprintf(b, sizeof b, "over-modulated (carrier pinched off %.2f %% of the time)", clipPct); add(b); }
    if (carrierShift) { snprintf(b, sizeof b, "carrier shift %+.1f %%", carrierShiftPct); add(b); }
    if (deadAir) { snprintf(b, sizeof b, "dead air for %.0f s", deadAirFor); add(b); }
    return s;
}

void AmHealth::configure(double rateHz, AmHealthParams p) { p_ = p; rate_ = rateHz; sec_.clear(); hist_.clear(); quietFor_ = 0; r_ = {}; }

void AmHealth::push(const float* e, size_t n) {
    if (rate_ <= 0) return;
    const size_t per = (size_t)std::max(1.0, std::round(rate_));
    for (size_t i = 0; i < n; i++) { sec_.push_back(e[i]); if (sec_.size() >= per) endSecond(); }
}

void AmHealth::endSecond() {
    double mean = 0;
    for (float v : sec_) mean += v;
    mean /= sec_.size();
    if (mean <= 0) { sec_.clear(); return; }
    std::vector<float> m(sec_.size());
    double s2 = 0; size_t clip = 0;
    for (size_t i = 0; i < sec_.size(); i++) {
        m[i] = (float)(sec_[i] / mean - 1.0);
        s2 += (double)m[i] * m[i];
        if (sec_[i] < 0.02 * mean) clip++;
    }
    const size_t k = std::min(m.size() - 1, (size_t)(0.999 * m.size()));
    std::vector<float> a = m;
    std::nth_element(a.begin(), a.begin() + k, a.end());
    r_.posPeakPct = 100.0 * a[k];
    for (auto& v : a) v = -v;
    std::nth_element(a.begin(), a.begin() + k, a.end());
    r_.negPeakPct = 100.0 * a[k];
    r_.modRmsPct = 100.0 * std::sqrt(s2 / m.size());
    hist_.push_back({mean, r_.modRmsPct, (double)clip / m.size()});
    if (hist_.size() > 60) hist_.pop_front();
    sec_.clear();

    double cs = 0; int cn = 0;
    for (size_t i = hist_.size() > 10 ? hist_.size() - 10 : 0; i < hist_.size(); i++) { cs += hist_[i].clipShare; cn++; }
    r_.clipPct = cn ? 100.0 * cs / cn : 0;
    r_.overModulated = r_.clipPct > p_.clipLimitPct;

    // carrier shift: mean level of the loudest third of the seconds against the quietest third
    if (hist_.size() >= 12) {
        std::vector<Sec> h(hist_.begin(), hist_.end());
        std::sort(h.begin(), h.end(), [](const Sec& x, const Sec& y) { return x.rms < y.rms; });
        const size_t t = h.size() / 3;
        double q = 0, l = 0;
        for (size_t i = 0; i < t; i++) { q += h[i].mean; l += h[h.size() - 1 - i].mean; }
        const bool spread = h[h.size() - 1 - t].rms - h[t].rms > 10;   // only meaningful when loud and quiet seconds differ
        r_.carrierShiftPct = spread && q > 0 ? 100.0 * (l - q) / q : 0;
        r_.carrierShift = std::fabs(r_.carrierShiftPct) > p_.shiftLimitPct;
    }
    if (r_.modRmsPct < p_.deadAirPct) quietFor_ += 1; else quietFor_ = 0;
    r_.deadAirFor = quietFor_;
    r_.deadAir = quietFor_ >= p_.deadAirSec;
}

} // namespace smart5
} // namespace dect2
