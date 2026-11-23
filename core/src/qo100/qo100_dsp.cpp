// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/qo100/qo100_dsp.h"
#include "dect2/dmr/dmr_dsp.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace qo100 {

const LoPreset* loPresets(int& count) {
    static const LoPreset k[] = {{"9750 MHz: ordinary Ku LNB (Bullseye, Octagon; IF 739.5 - 750 MHz)", 9750e6}, {"9739 MHz: LNB with a 25 MHz TCXO or GPS lock (IF 750.5 - 761 MHz)", 9739e6},
                                 {"10000 MHz: 10 MHz-locked LNB (IF 489.5 - 500 MHz)", 10000e6}, {"10600 MHz: universal LNB, high band (IF 100 - 110.5 MHz, inverted)", 10600e6}};
    count = (int)(sizeof k / sizeof k[0]);
    return k;
}

void Nco::set(double hz, double fs) {
    hz_ = hz;
    const double w = fs > 0 ? -2.0 * M_PI * hz / fs : 0.0;
    step_ = std::complex<double>(std::cos(w), std::sin(w));
}

void Decimator::configure(double inRate, double passHz, double minOutRate) {
    st_.clear();
    double r = inRate;
    while (r / 2 >= minOutRate && r / 2 > 2.2 * passHz) {
        Stage s;
        s.h = dmr::lowpassTaps(passHz, r / 2 - passHz, r, 70, 9, 255);
        s.hist.assign(s.h.size() - 1, cf32(0, 0));   // the last T - 1 input samples
        st_.push_back(std::move(s));
        r /= 2;
    }
    out_ = r;
}

void Decimator::reset() {
    for (Stage& s : st_) { std::fill(s.hist.begin(), s.hist.end(), cf32(0, 0)); s.ph = 0; }
}

void Decimator::process(const cf32* x, size_t n, std::vector<cf32>& out) {
    if (st_.empty()) { out.insert(out.end(), x, x + n); return; }
    a_.assign(x, x + n);
    for (size_t si = 0; si < st_.size(); si++) {
        Stage& s = st_[si];
        const size_t T = s.h.size(), m = a_.size();
        std::vector<cf32>& dst = si + 1 == st_.size() ? out : b_;
        if (&dst == &b_) b_.clear();
        // the last T - 1 inputs of the previous call, then this call's: the window of the input at index j is ext[j .. j + T)
        ext_.resize(T - 1 + m);
        std::copy(s.hist.begin(), s.hist.end(), ext_.begin());
        std::copy(a_.begin(), a_.end(), ext_.begin() + (std::ptrdiff_t)(T - 1));
        const size_t first = s.ph == 0 ? 1 : 0;   // an output after every second input
        const size_t nOut = m > first ? (m - first + 1) / 2 : 0;
        const size_t base = dst.size();
        dst.resize(base + nOut);
        const float* h = s.h.data();
        size_t o = 0;
        // four outputs at a time: each keeps its own sequential sum (the result is the one-output-at-a-time value bit for bit), the four chains overlap
        for (; o + 4 <= nOut; o += 4) {
            const cf32* e = &ext_[first + 2 * o];
            float ar[4] = {0, 0, 0, 0}, ai[4] = {0, 0, 0, 0};
            for (size_t k = 0; k < T; k++) {
                const float hk = h[k];
                for (int q = 0; q < 4; q++) { ar[q] += hk * e[k + (size_t)(2 * q)].real(); ai[q] += hk * e[k + (size_t)(2 * q)].imag(); }
            }
            for (int q = 0; q < 4; q++) dst[base + o + (size_t)q] = cf32(ar[q], ai[q]);
        }
        for (; o < nOut; o++) {
            const cf32* e = &ext_[first + 2 * o];
            float ar = 0, ai = 0;
            for (size_t k = 0; k < T; k++) { ar += h[k] * e[k].real(); ai += h[k] * e[k].imag(); }
            dst[base + o] = cf32(ar, ai);
        }
        std::copy(ext_.end() - (std::ptrdiff_t)(T - 1), ext_.end(), s.hist.begin());
        s.ph = (int)((s.ph + m) & 1);
        if (&dst == &b_) std::swap(a_, b_);
    }
}

static double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + (long)(v.size() / 2), v.end());
    return v[v.size() / 2];
}

// the offset (in bins, -0.5 .. 0.5) of the top of a parabola through three log powers
static double parabola(double a, double b, double c) {
    const double la = 10 * std::log10(a + 1e-30), lb = 10 * std::log10(b + 1e-30), lc = 10 * std::log10(c + 1e-30);
    const double d = la - 2 * lb + lc;
    return d < 0 ? std::clamp(0.5 * (la - lc) / d, -0.5, 0.5) : 0.0;
}

Peak strongest(const std::vector<double>& pow, double rate, double maxHz) {
    Peak p;
    const int N = (int)pow.size();
    if (N < 8 || rate <= 0) return p;
    const double bin = rate / N;
    const int span = std::min(N / 2 - 2, (int)(maxHz / bin));
    int best = -1;
    for (int k = N / 2 - span; k <= N / 2 + span; k++) if (best < 0 || pow[(size_t)k] > pow[(size_t)best]) best = k;
    if (best <= 0 || best >= N - 1) return p;
    const double med = median(pow);
    const double d = parabola(pow[(size_t)best - 1], pow[(size_t)best], pow[(size_t)best + 1]);
    p.hz = (best + d - N / 2) * bin;
    p.levelDb = 10 * std::log10(pow[(size_t)best] + 1e-30);
    p.snrDb = 10 * std::log10((pow[(size_t)best] + 1e-30) / (med + 1e-30));
    p.ok = true;
    return p;
}

BeaconFit findBeacons(const std::vector<double>& pow, double f0Hz, double binHz, double maxShiftHz, double minDb) {
    BeaconFit r;
    const int N = (int)pow.size();
    if (N < 16 || binHz <= 0) return r;
    const double med = median(pow);
    r.floorDb = 10 * std::log10(med + 1e-30);
    auto at = [&](int k) { double m = 0; for (int j = k - 1; j <= k + 1; j++) if (j >= 0 && j < N) m = std::max(m, pow[(size_t)j]); return m; };
    const int steps = (int)(maxShiftHz / binHz);
    double bestScore = 0; int bestS = 0; bool any = false;
    for (int s = -steps; s <= steps; s++) {
        const double e = s * binHz;
        const int kl = (int)std::lround((kLowBeaconHz + e - f0Hz) / binHz), kh = (int)std::lround((kHighBeaconHz + e - f0Hz) / binHz);
        if (kl < 2 || kh > N - 3) continue;
        const double sc = std::min(at(kl), at(kh));
        if (!any || sc > bestScore) { bestScore = sc; bestS = s; any = true; }
    }
    if (!any || 10 * std::log10((bestScore + 1e-30) / (med + 1e-30)) < minDb) return r;
    // refine each beacon on its own top bin
    double fsum = 0;
    double lv[2] = {0, 0};
    for (int b = 0; b < 2; b++) {
        const double nom = b ? kHighBeaconHz : kLowBeaconHz;
        const int k0 = (int)std::lround((nom + bestS * binHz - f0Hz) / binHz);
        int k = k0;
        for (int j = k0 - 1; j <= k0 + 1; j++) if (j > 0 && j < N - 1 && pow[(size_t)j] > pow[(size_t)k]) k = j;
        if (k <= 0 || k >= N - 1) return r;
        const double f = f0Hz + (k + parabola(pow[(size_t)k - 1], pow[(size_t)k], pow[(size_t)k + 1])) * binHz;
        fsum += f - nom;
        lv[b] = 10 * std::log10(pow[(size_t)k] + 1e-30);
    }
    r.ok = true;
    r.shiftHz = fsum / 2;
    r.lowDb = lv[0]; r.highDb = lv[1];
    return r;
}

void BeaconTracker::reset() { *this = BeaconTracker(); }

double BeaconTracker::gateHz() const {
    if (inno_.size() < 6) return 15;
    std::vector<double> a(inno_.size());
    for (size_t i = 0; i < a.size(); i++) a[i] = std::fabs(inno_[i]);
    std::nth_element(a.begin(), a.begin() + (long)(a.size() / 2), a.end());
    return std::clamp(5 * 1.4826 * a[a.size() / 2], kGateMinHz, kGateMaxHz);
}

bool BeaconTracker::accept(double t, double z) {
    if (!init_) { est_ = z; rate_ = 0; tEst_ = t; out_ = z; tOut_ = t; init_ = true; locked_ = true; missRun_ = 0; inno_.clear(); nAcc_ = 1; return true; }
    const double dt = std::clamp(t - tEst_, 0.05, 5.0);
    const double p = est_ + rate_ * dt, r = z - p;
    const double a = nAcc_ < 4 ? 0.6 : 0.3, b = nAcc_ < 4 ? 0.15 : 0.04;
    est_ = p + a * r;
    rate_ = std::clamp(rate_ + b / dt * r, -20.0, 20.0);
    tEst_ = t;
    inno_.push_back(r);
    if (inno_.size() > 16) inno_.erase(inno_.begin());
    nAcc_++; missRun_ = 0; locked_ = true;
    return true;
}

bool BeaconTracker::update(double t, const BeaconMeas m[2]) {
    const bool g0 = m[0].ok, g1 = m[1].ok;
    if (!g0 && !g1) { miss(t); return false; }
    if (init_) {
        const double p = est_ + rate_ * std::clamp(t - tEst_, 0.0, 30.0), gate = gateHz();
        const bool in0 = m[0].ok && std::fabs(m[0].errHz - p) <= gate, in1 = m[1].ok && std::fabs(m[1].errHz - p) <= gate;
        if (in0 || in1) {
            double z = in0 ? m[0].errHz : m[1].errHz;
            if (in0 && in1) z = std::fabs(m[0].errHz - m[1].errHz) <= 6 ? 0.5 * (m[0].errHz + m[1].errHz) : std::fabs(m[0].errHz - p) <= std::fabs(m[1].errHz - p) ? m[0].errHz : m[1].errHz;   // two that disagree: the one nearer the prediction (the other has a carrier on it)
            candN_ = 0;
            return accept(t, z);
        }
    }
    // no estimate yet, or both beacons are off the prediction: a candidate for a new level, taken when they keep agreeing
    const bool two = g0 && g1 && std::fabs(m[0].errHz - m[1].errHz) < 12;
    if (g0 && g1 && !two) { candN_ = 0; miss(t); return false; }      // the beacons disagree: something else than the beacons
    const double z = two ? 0.5 * (m[0].errHz + m[1].errHz) : g0 ? m[0].errHz : m[1].errHz;
    if (candN_ > 0 && std::fabs(z - candSum_ / candN_) < 15) { candN_++; candSum_ += z; candTwo_ = candTwo_ && two; }
    else { candN_ = 1; candSum_ = z; candTwo_ = two; }
    const int need = init_ ? (candTwo_ ? 4 : 10) : (candTwo_ ? 3 : 5);
    if (candN_ >= need) {
        const double v = candSum_ / candN_;
        candN_ = 0;
        if (init_) { est_ = v; tEst_ = t; inno_.clear(); nAcc_ = 4; missRun_ = 0; locked_ = true; return true; }   // a step: keep the drift, restart the gate
        accept(t, v);
        return true;
    }
    miss(t);
    return false;
}

double BeaconTracker::value(double t) {
    if (!init_) return 0;
    const double coast = std::clamp(t - tEst_, 0.0, 30.0);   // the drift is carried on for up to 30 s without a measurement
    const double target = est_ + rate_ * coast;
    const double dt = std::max(0.0, t - tOut_);
    tOut_ = std::max(tOut_, t);
    const double lim = kSlewHzPerSec * dt;
    out_ += std::clamp(target - out_, -lim, lim);
    return out_;
}

bool slopePerMin(const std::vector<double>& t, const std::vector<double>& v, double& perMin) {
    const size_t n = std::min(t.size(), v.size());
    if (n < 3) return false;
    double mt = 0, mv = 0;
    for (size_t i = 0; i < n; i++) { mt += t[i]; mv += v[i]; }
    mt /= (double)n; mv /= (double)n;
    double sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; i++) { sxx += (t[i] - mt) * (t[i] - mt); sxy += (t[i] - mt) * (v[i] - mv); }
    if (sxx < 1e-9) return false;
    perMin = sxy / sxx * 60.0;
    return true;
}

} // namespace qo100
} // namespace dect2
