// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/tx/tx_measure.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace dect2 { namespace txm {

namespace {
const double kPi = 3.14159265358979323846;
using cd = std::complex<double>;

void fft(std::vector<cd>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * kPi / (double)len;
        const cd wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cd w(1, 0);
            for (size_t k = 0; k < len / 2; k++) {
                const cd u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
                if ((k & 63) == 63) w = cd(std::cos(ang * (double)(k + 1)), std::sin(ang * (double)(k + 1)));   // keep the twiddle exact on long runs
            }
        }
    }
}

double uni(std::mt19937& g) { return ((double)g() + 0.5) / 4294967296.0; }
double gauss(std::mt19937& g) { return std::sqrt(-2.0 * std::log(uni(g))) * std::cos(2 * kPi * uni(g)); }

double meanPower(const std::vector<cf32>& x) {
    double s = 0;
    for (const auto& v : x) s += (double)v.real() * v.real() + (double)v.imag() * v.imag();
    return x.empty() ? 0 : s / (double)x.size();
}
}

double Psd::db(size_t bin) const { return bin < pw.size() && pw[bin] > 1e-20 ? 10.0 * std::log10(pw[bin]) : -200.0; }

Psd welchPsd(const cf32* x, size_t n, double rateHz, int nfft, int maxSeg) {
    Psd p;
    if (!x || n < 8 || rateHz <= 0) return p;
    int nf = 8;
    while (nf < nfft) nf <<= 1;
    p.rateHz = rateHz;
    p.binHz = rateHz / nf;
    p.pw.assign((size_t)nf, 0.0);
    const size_t L = std::min(n, (size_t)nf);              // window length (shorter than the FFT for a short block: zero padded)
    std::vector<double> w(L);
    double sw2 = 0;
    for (size_t i = 0; i < L; i++) { w[i] = 0.5 - 0.5 * std::cos(2 * kPi * ((double)i + 0.5) / (double)L); sw2 += w[i] * w[i]; }
    size_t nseg = 1, step = 1;
    if (n > L) {
        const size_t hop = L / 2, avail = (n - L) / hop + 1;
        nseg = std::min(avail, (size_t)std::max(1, maxSeg));
        step = nseg > 1 ? (n - L) / (nseg - 1) : 1;           // spread the segments over the whole block
    }
    std::vector<cd> a((size_t)nf);
    for (size_t s = 0; s < nseg; s++) {
        const cf32* seg = x + s * step;
        for (size_t i = 0; i < (size_t)nf; i++) a[i] = i < L ? cd(seg[i].real() * w[i], seg[i].imag() * w[i]) : cd(0, 0);
        fft(a);
        for (size_t k = 0; k < (size_t)nf; k++) p.pw[(k + (size_t)nf / 2) % (size_t)nf] += std::norm(a[k]);
    }
    const double norm = 1.0 / ((double)nseg * (double)nf * sw2);
    for (auto& v : p.pw) v *= norm;
    return p;
}

double obwHz(const Psd& p, double fraction) {
    if (p.empty()) return 0;
    double tot = 0;
    for (double v : p.pw) tot += v;
    if (tot <= 0) return 0;
    const double cut = tot * (1.0 - std::min(1.0, std::max(0.0, fraction))) / 2.0;
    size_t a = 0, b = p.pw.size() - 1;
    double lo = 0, hi = 0;
    while (a < b && lo + p.pw[a] <= cut) lo += p.pw[a++];
    while (b > a && hi + p.pw[b] <= cut) hi += p.pw[b--];
    return (double)(b - a + 1) * p.binHz;
}

double obwBelowPeakHz(const Psd& p, double dbBelowPeak, double smoothHz) {
    if (p.empty()) return 0;
    const size_t n = p.pw.size();
    size_t m = smoothHz > 0 ? (size_t)std::max(1.0, std::round(smoothHz / p.binHz)) : 8;
    m = std::min(m, n);
    std::vector<double> pre(n + 1, 0.0), s(n, 0.0);
    for (size_t i = 0; i < n; i++) pre[i + 1] = pre[i] + p.pw[i];
    for (size_t i = 0; i < n; i++) {                          // moving average over m bins, centred
        const size_t lo = i >= m / 2 ? i - m / 2 : 0, hi = std::min(n, lo + m);
        s[i] = (pre[hi] - pre[lo]) / (double)(hi - lo);
    }
    double pk = 0;
    for (double v : s) pk = std::max(pk, v);
    if (pk <= 0) return 0;
    const double thr = pk * std::pow(10.0, -dbBelowPeak / 10.0);
    size_t a = n, b = 0;
    for (size_t i = 0; i < n; i++) if (s[i] >= thr) { a = std::min(a, i); b = i; }
    return a > b ? 0 : (double)(b - a + 1) * p.binHz;
}

double centroidHz(const Psd& p) {
    double tot = 0, acc = 0;
    for (size_t i = 0; i < p.pw.size(); i++) { tot += p.pw[i]; acc += p.pw[i] * p.freqOf(i); }
    return tot > 0 ? acc / tot : 0;
}

Papr papr(const cf32* x, size_t n) {
    Papr r;
    if (!x || n < 2) return r;
    double pk = 0, sum = 0;
    for (size_t i = 0; i < n; i++) {
        const double p = (double)x[i].real() * x[i].real() + (double)x[i].imag() * x[i].imag();
        pk = std::max(pk, p);
        sum += p;
    }
    if (sum <= 0) return r;
    const double ms = sum / (double)n;
    r.peak = std::sqrt(pk);
    r.peakDb = 10.0 * std::log10(pk);
    r.rmsDb = 10.0 * std::log10(ms);
    r.paprDb = r.peakDb - r.rmsDb;
    return r;
}

double paprAtProbabilityDb(const cf32* x, size_t n, double prob) {
    if (!x || n < 2) return 0;
    std::vector<double> pw(n);
    double sum = 0;
    for (size_t i = 0; i < n; i++) { pw[i] = (double)x[i].real() * x[i].real() + (double)x[i].imag() * x[i].imag(); sum += pw[i]; }
    if (sum <= 0) return 0;
    const size_t k = std::min(n - 1, (size_t)std::floor((1.0 - std::min(1.0, std::max(0.0, prob))) * (double)n));
    std::nth_element(pw.begin(), pw.begin() + (long)k, pw.end());
    return 10.0 * std::log10(std::max(pw[k], 1e-30) / (sum / (double)n));
}

namespace {
// power of the bins whose centre lies inside [c - w/2, c + w/2]; false when that range leaves the sampled span
bool bandPower(const Psd& p, double c, double w, double& out) {
    const double half = p.rateHz / 2;
    if (c - w / 2 < -half || c + w / 2 > half) return false;
    double s = 0;
    for (size_t i = 0; i < p.pw.size(); i++) {
        const double f = p.freqOf(i);
        if (f >= c - w / 2 && f <= c + w / 2) s += p.pw[i];
    }
    out = s;
    return true;
}
}

Acpr acpr(const Psd& p, double chanHz, double offsetHz, double centerHz) {
    Acpr r;
    r.centerHz = centerHz;
    if (p.empty() || chanHz <= 0) return r;
    double own = 0, lo = 0, hi = 0;
    if (!bandPower(p, centerHz, chanHz, own) || own <= 0) return r;
    if (bandPower(p, centerHz - offsetHz, chanHz, lo)) { r.lowerValid = true; r.lowerDb = 10.0 * std::log10(std::max(lo, 1e-30) / own); }
    if (bandPower(p, centerHz + offsetHz, chanHz, hi)) { r.upperValid = true; r.upperDb = 10.0 * std::log10(std::max(hi, 1e-30) / own); }
    return r;
}

double Mask::limitAt(double offsetHz) const {
    if (pts.empty()) return 0;
    const double o = std::fabs(offsetHz);
    if (o <= pts.front().offsetHz) return pts.front().limitDb;
    for (size_t i = 1; i < pts.size(); i++)
        if (o <= pts[i].offsetHz) {
            const double t = (o - pts[i - 1].offsetHz) / std::max(1e-9, pts[i].offsetHz - pts[i - 1].offsetHz);
            return pts[i - 1].limitDb + t * (pts[i].limitDb - pts[i - 1].limitDb);
        }
    return pts.back().limitDb;
}

Mask genericMask(double bw) {
    Mask m;
    m.name = "generic shoulder (not a standard's mask)";
    m.regulatory = false;
    m.pts = {{0, 0}, {0.5 * bw, 0}, {0.75 * bw, -30}, {1.5 * bw, -50}};
    return m;
}

Mask maskForMode(const char*, double bw) { return genericMask(bw); }

MaskResult checkMask(const Psd& p, const Mask& m, double bw, double centerHz) {
    MaskResult r;
    if (p.empty() || bw <= 0) return r;
    double sum = 0;
    size_t cnt = 0;
    for (size_t i = 0; i < p.pw.size(); i++)
        if (std::fabs(p.freqOf(i) - centerHz) <= bw / 2) { sum += p.pw[i]; cnt++; }
    if (cnt == 0 || sum <= 0) return r;
    r.refDb = 10.0 * std::log10(sum / (double)cnt);
    const double half = p.rateHz / 2;
    const double room = std::max(half - centerHz, half + centerHz);
    if (room < 0.75 * bw) return r;                           // the span does not reach past the band: nothing to judge
    r.valid = true;
    for (size_t i = 0; i < p.pw.size(); i++) {
        const double off = p.freqOf(i) - centerHz;
        if (std::fabs(off) <= bw / 2) continue;
        const double ex = p.db(i) - r.refDb - m.limitAt(off);
        if (ex > r.worstExcessDb) { r.worstExcessDb = ex; r.atHz = off; }
    }
    r.pass = r.worstExcessDb <= 0;
    return r;
}

void applyImpairments(std::vector<cf32>& x, double rateHz, const Impair& im) {
    if (x.empty() || rateHz <= 0 || !im.any()) return;
    std::mt19937 g(im.seed);
    const size_t n = x.size();
    const double p0 = meanPower(x);
    if (im.fading) {
        constexpr int N = 16;
        auto makePaths = [&](double ph[N], double wd[N]) {
            for (int k = 0; k < N; k++) {
                const double alpha = 2 * kPi * ((double)k + uni(g)) / N;
                wd[k] = 2 * kPi * im.dopplerHz * std::cos(alpha) / rateHz;
                ph[k] = 2 * kPi * uni(g);
            }
        };
        double ph1[N], wd1[N], ph2[N], wd2[N];
        makePaths(ph1, wd1);
        makePaths(ph2, wd2);
        const size_t d = std::max<size_t>(1, (size_t)std::lround(im.delayUs * 1e-6 * rateHz));
        std::vector<cf32> y(n);
        auto h = [&](const double* ph, const double* wd, size_t i) {
            cd s(0, 0);
            const double t = (double)i;
            for (int k = 0; k < N; k++) s += cd(std::cos(wd[k] * t + ph[k]), std::sin(wd[k] * t + ph[k]));
            return s / std::sqrt((double)N);
        };
        for (size_t i = 0; i < n; i++) {
            cd v = h(ph1, wd1, i) * cd(x[i].real(), x[i].imag());
            if (im.fading == 2 && i >= d) v += 0.5 * h(ph2, wd2, i) * cd(x[i - d].real(), x[i - d].imag());
            y[i] = cf32((float)v.real(), (float)v.imag());
        }
        const double p1 = meanPower(y);
        const float k = p1 > 0 ? (float)std::sqrt(p0 / p1) : 1.f;   // the mean power stays what it was
        for (size_t i = 0; i < n; i++) x[i] = y[i] * k;
    }
    if (im.cfoHz != 0) {
        const double w = 2 * kPi * im.cfoHz / rateHz;
        for (size_t i = 0; i < n; i++) {
            const double a = w * (double)i;
            x[i] *= cf32((float)std::cos(a), (float)std::sin(a));
        }
    }
    if (im.snrDb < 99) {
        const double sigma = std::sqrt(std::max(p0, 1e-30) / std::pow(10.0, im.snrDb / 10.0) / 2.0);
        for (size_t i = 0; i < n; i++) x[i] += cf32((float)(sigma * gauss(g)), (float)(sigma * gauss(g)));
    }
}

}} // namespace dect2::txm
