// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Product cadence watchdog (see product_cadence.h).
#include "dect2/smart5/product_cadence.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace smart5 {

void ProductCadence::product(const std::string& name, double t) {
    P& p = p_[name];
    p.s.name = name;
    p.arr.push_back(t);
    while (p.arr.size() > 30) p.arr.pop_front();
    // the period: the median interval, with intervals that span missed products divided down to the base period
    if (p.arr.size() >= 4) {
        std::vector<double> d;
        for (size_t i = 1; i < p.arr.size(); i++) d.push_back(p.arr[i] - p.arr[i - 1]);
        std::vector<double> s(d);
        std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
        const double med = s[s.size() / 2];
        double num = 0, den = 0;
        for (double x : d) { const double k = std::round(x / med); if (k >= 1 && std::fabs(x - k * med) < 0.2 * med) { num += x; den += k; } }
        if (den > 0) p.s.periodSec = num / den;
    }
    p.s.last = t;
    p.s.nextDue = p.s.periodSec > 0 ? t + p.s.periodSec : 0;
    p.s.missing = MissCause::None;
}

void ProductCadence::link(double t, bool locked, bool framesOk) {
    link_.push_back({t, locked && framesOk});
    while (!link_.empty() && link_.front().first < t - 86400) link_.pop_front();
}

bool ProductCadence::linkGoodBetween(double a, double b) const {
    int good = 0, all = 0;
    for (auto& l : link_) if (l.first >= a && l.first <= b) { all++; if (l.second) good++; }
    return all > 0 && good >= 0.95 * all;
}

void ProductCadence::tick(double now) {
    for (auto& kv : p_) {
        ProductState& s = kv.second.s;
        if (s.periodSec <= 0) continue;
        // every due time that passed without the product: judge the window around it, then expect the next one
        while (now >= s.nextDue + grace_ * s.periodSec) {
            const bool good = linkGoodBetween(s.nextDue - 0.25 * s.periodSec, s.nextDue + grace_ * s.periodSec);
            s.missing = good ? MissCause::NotSent : MissCause::LostLock;
            s.missedCount++;
            s.nextDue += s.periodSec;
        }
    }
}

std::vector<ProductState> ProductCadence::states() const {
    std::vector<ProductState> v;
    for (auto& kv : p_) v.push_back(kv.second.s);
    return v;
}

std::string ProductCadence::text(const std::string& name) const {
    auto it = p_.find(name);
    if (it == p_.end() || it->second.s.missing == MissCause::None) return "";
    const ProductState& s = it->second.s;
    char b[200];
    snprintf(b, sizeof b, "%s overdue (every %.0f min): %s", name.c_str(), s.periodSec / 60,
             s.missing == MissCause::NotSent ? "lock held and frames arrived - the satellite did not send it" : "you lost lock while it was due");
    return b;
}

} // namespace smart5
} // namespace dect2
