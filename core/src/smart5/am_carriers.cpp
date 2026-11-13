// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AM split-carrier analyser (see am_carriers.h).
#include "dect2/smart5/am_carriers.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace smart5 {

void AmCarrierAnalyser::configure(double rateHz, const AmCarrierParams& p, double) {
    p_ = p; rate_ = rateHz;
    D_ = std::max(1, (int)std::floor(rateHz / (2.5 * p.spanHz)));
    decRate_ = rateHz / D_;
    L_ = 1;
    while (L_ < decRate_ / p.resolutionHz) L_ <<= 1;
    binHz_ = decRate_ / L_;
    averages_ = std::max(1, p.averages);
    seg_.assign(L_, {0, 0}); pow_.assign(L_, 0); win_.resize(L_);
    for (int i = 0; i < L_; i++) win_[i] = 0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * i / L_);
    have_ = segs_ = 0; acc_ = {0, 0}; accN_ = 0; out_.clear(); hist_.clear(); fresh_ = false;
}

void AmCarrierAnalyser::push(const std::complex<float>* x, size_t n, double now) {
    if (!L_) return;
    for (size_t i = 0; i < n; i++) {
        acc_ += std::complex<double>(x[i].real(), x[i].imag());
        if (++accN_ < D_) continue;
        seg_[have_++] = std::complex<float>((float)(acc_.real() / D_), (float)(acc_.imag() / D_));
        acc_ = {0, 0}; accN_ = 0;
        if (have_ < L_) continue;
        have_ = 0;
        std::vector<cf32> b(L_);
        for (int k = 0; k < L_; k++) b[k] = cf32(seg_[k].real() * (float)win_[k], seg_[k].imag() * (float)win_[k]);
        Fft f(L_);
        f.forward(b.data());
        for (int k = 0; k < L_; k++) pow_[k] += std::norm(std::complex<double>(b[k].real(), b[k].imag()));
        if (++segs_ >= averages_) { analyse(now); std::fill(pow_.begin(), pow_.end(), 0.0); segs_ = 0; }
    }
}

void AmCarrierAnalyser::analyse(double now) {
    // bins within the span, as offsets: k = 0 .. L/2-1 positive, L/2 .. L-1 negative
    const int half = std::min(L_ / 2 - 2, (int)(p_.spanHz / binHz_));
    std::vector<double> db(2 * half + 1);
    for (int j = -half; j <= half; j++) db[j + half] = 10 * std::log10(pow_[(j + L_) % L_] + 1e-30);
    std::vector<double> s = db;
    std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
    const double floorDb = s[s.size() / 2];
    std::vector<AmCarrier> found;
    for (int i = 2; i + 2 < (int)db.size(); i++) {
        const double v = db[i];
        if (v - floorDb < p_.minSnrDb) continue;
        if (!(v > db[i - 1] && v >= db[i + 1] && v > db[i - 2] && v >= db[i + 2])) continue;
        const double a = db[i - 1], c = db[i + 1], den = a - 2 * v + c;
        const double d = den < 0 ? 0.5 * (a - c) / den : 0;
        AmCarrier ca;
        ca.offsetHz = (i - half + d) * binHz_;
        ca.snrDb = v - 0.25 * (a - c) * d - floorDb;
        found.push_back(ca);
    }
    std::sort(found.begin(), found.end(), [](const AmCarrier& x, const AmCarrier& y) { return x.snrDb > y.snrDb; });
    // a Hann main lobe spans +/- 2 bins: drop side peaks of a strong carrier closer than 3 bins
    std::vector<AmCarrier> keep;
    for (auto& c : found) {
        bool nearV = false;
        for (auto& k : keep) if (std::fabs(k.offsetHz - c.offsetHz) < 3 * binHz_) nearV = true;
        if (!nearV) keep.push_back(c);
    }
    if (keep.size() > 16) keep.resize(16);
    // history: match to the previous carriers within 1 Hz
    std::vector<std::vector<Hist>> nh;
    for (auto& c : keep) {
        std::vector<Hist> h;
        double best = 1.0;
        for (auto& old : hist_) if (!old.empty() && std::fabs(old.back().off - c.offsetHz) < best) { best = std::fabs(old.back().off - c.offsetHz); h = old; }
        h.push_back({c.offsetHz, now});
        if (h.size() > 30) h.erase(h.begin());
        c.seen = (int)h.size();
        if (h.size() >= 2 && h.back().t - h.front().t > 0) {
            double mt = 0, mo = 0;
            for (auto& x : h) { mt += x.t; mo += x.off; }
            mt /= h.size(); mo /= h.size();
            double sxy = 0, sxx = 0;
            for (auto& x : h) { sxy += (x.t - mt) * (x.off - mo); sxx += (x.t - mt) * (x.t - mt); }
            c.driftHzPerMin = sxx > 0 ? sxy / sxx * 60 : 0;
        }
        nh.push_back(h);
    }
    hist_ = nh;
    if (!keep.empty()) {
        const double top = keep[0].snrDb, f0 = keep[0].offsetHz;
        for (auto& c : keep) {
            c.levelDb = c.snrDb - top;
            c.hetHz = std::fabs(c.offsetHz - f0);
            for (auto& b : book_) if (std::fabs(b.offsetHz - c.offsetHz) <= b.tolHz) { c.name = b.name; break; }
        }
    }
    out_ = keep;
    fresh_ = true;
}

} // namespace smart5
} // namespace dect2
