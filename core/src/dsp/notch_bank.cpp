// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/dsp/notch_bank.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dect2 {

double NotchBank::clampWidth(double w) {
    if (!std::isfinite(w)) return kDefaultWidthHz;
    return std::max(kMinWidthHz, std::min(kMaxWidthHz, w));
}

void NotchBank::set(const std::vector<Notch>& v) {
    std::lock_guard<std::mutex> lk(mu_);
    pending_.clear();
    for (const Notch& n : v) {
        if ((int)pending_.size() >= kMax) break;
        Notch c = n;
        c.widthHz = clampWidth(c.widthHz);
        pending_.push_back(c);
    }
    int on = 0;
    for (const Notch& n : pending_) if (n.enabled) on++;
    count_.store(on, std::memory_order_relaxed);
    dirty_.store(true, std::memory_order_release);
}

std::vector<Notch> NotchBank::get() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pending_;
}

void NotchBank::process(cf32* x, size_t n, double rateHz, double centreHz) {
    if (dirty_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lk(mu_);
        dirty_.store(false, std::memory_order_relaxed);
        act_ = pending_;
        st_.resize(act_.size());   // new entries start empty; an index that stays keeps its state
    }
    if (count_.load(std::memory_order_relaxed) == 0 || act_.empty() || n == 0 || !(rateHz > 0)) return;
    const double pi = 3.14159265358979323846;
    for (size_t k = 0; k < act_.size(); k++) {
        const Notch& nt = act_[k];
        if (!nt.enabled) continue;
        const double f = basebandHz(nt, centreHz);
        if (!std::isfinite(f) || std::fabs(f) > 0.48 * rateHz) { st_[k] = St(); continue; }   // outside the capture: nothing to cut (and no stale state)
        const double w = 2 * pi * f / rateHz, cr = std::cos(w), ci = std::sin(w);
        const double r = std::max(0.0, 1.0 - pi * nt.widthHz / rateHz);
        St& s = st_[k];
        for (int stage = 0; stage < 2; stage++) {
            // y[n] = x[n] - c * (x[n-1] - r * y[n-1]) ; the state keeps u = x[n-1] - r * y[n-1]
            double ur = s.ur[stage], ui = s.ui[stage];
            for (size_t i = 0; i < n; i++) {
                const double xr = x[i].real(), xi = x[i].imag();
                const double yr = xr - (cr * ur - ci * ui), yi = xi - (cr * ui + ci * ur);
                ur = xr - r * yr; ui = xi - r * yi;
                x[i] = cf32((float)yr, (float)yi);
            }
            s.ur[stage] = ur; s.ui[stage] = ui;
        }
    }
}

std::string NotchBank::toText(const std::vector<Notch>& v) {
    std::string t = "1;";
    for (size_t i = 0; i < v.size(); i++) {
        char b[96];
        snprintf(b, sizeof b, "%s%.3f,%.1f,%d,%d", i ? "|" : "", v[i].follow ? v[i].offsetHz : v[i].rfHz, v[i].widthHz, v[i].follow ? 1 : 0, v[i].enabled ? 1 : 0);
        t += b;
    }
    return t;
}

std::vector<Notch> NotchBank::fromText(const std::string& s) {
    std::vector<Notch> out;
    if (s.size() < 2 || s[0] != '1' || s[1] != ';') return out;
    size_t p = 2;
    while (p < s.size() && (int)out.size() < kMax) {
        size_t e = s.find('|', p);
        if (e == std::string::npos) e = s.size();
        const std::string item = s.substr(p, e - p);
        double f = 0, w = kDefaultWidthHz; int fo = 0, en = 1;
        if (sscanf(item.c_str(), "%lf,%lf,%d,%d", &f, &w, &fo, &en) >= 2 && std::isfinite(f)) {
            Notch n;
            n.follow = fo != 0; n.enabled = en != 0; n.widthHz = clampWidth(w);
            if (n.follow) n.offsetHz = f; else n.rfHz = f;
            out.push_back(n);
        }
        p = e + 1;
    }
    return out;
}

} // namespace dect2
