// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ROS identifier (ros_detect.h): one tone at a time on a fine grid, ~2.25 kHz wide, 4 / 8 / 16 baud. Clean-room, from public descriptions only
// (docs/ROS.md); nothing of the ROS program was used.
//
// How: mix the signal's centre to 0 Hz, filter and decimate to about 6 kHz, then a short-time spectrum (40 ms Hann, 10 ms hop). A frame whose
// power sits in one tone gives that tone's frequency (parabolic peak); consecutive frames on the same tone form a segment (about one symbol).
// The segment frequencies give the tone grid (the largest spacing on which they all sit, by the vector strength of f / spacing), the span and the
// number of grid positions used; the segment lengths give the symbol rate.
#include "dect2/ros/ros_detect.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>

namespace dect2 {
namespace ros {

namespace {
constexpr double kPi = 3.14159265358979323846;

const SubModeInfo kSub[] = {
    {"ros16", "ROS 16 baud", 16, 2250, true, "sigidwiki ROS (ROS-2K 4/8/16, RX16); radioscanner.ru signal 236 (~15.5 Bd, ~2250-2300 Hz)"},
    {"ros8", "ROS 8 baud", 8, 2250, true, "sigidwiki ROS (ROS-2K 4/8/16, RX8); calvibit.net IK1AMC (16, 8 and 4 baud in 2.2 kHz)"},
    {"ros4", "ROS 4 baud", 4, 2250, true, "sigidwiki ROS (ROS-2K 4/8/16, RX4); calvibit.net IK1AMC"},
    {"ros1", "ROS 1 baud (older versions)", 1, 2200, false, "amateurradio.com 'Yet Another Digimode' (16 or 1 baud, 2.2 kHz); radioscanner.ru (very slow mode 1)"},
    {"rosmf", "ROS MF (MF1 / MF2 / MF7)", 0, 100, false, "sigidwiki ROS (100 Hz, UK 500 kHz trials)"},
    {"roseme", "ROS EME (VHF)", 0, 50, false, "sigidwiki ROS (50 Hz, 144 MHz EME, 1500 Hz AFC range)"},
    {"ros500", "ROS HF 500 Hz", 0, 500, false, "sigidwiki ROS (narrow 500 Hz version)"},
};

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

// three cascaded moving averages of length L (a sinc^3: the neighbours that fold onto the band are 80 dB down), then every L-th sample
std::vector<cf32> boxDecimate(std::vector<cf32> y, size_t L) {
    if (L <= 1) return y;
    for (int pass = 0; pass < 3; pass++) {
        std::vector<cf32> ring(L, cf32(0, 0));
        std::complex<double> s(0, 0);
        for (size_t i = 0; i < y.size(); i++) {
            const cf32 v = y[i];
            s += std::complex<double>(v.real(), v.imag()) - std::complex<double>(ring[i % L].real(), ring[i % L].imag());
            ring[i % L] = v;
            y[i] = cf32((float)(s.real() / L), (float)(s.imag() / L));
        }
    }
    std::vector<cf32> o;
    o.reserve(y.size() / L + 1);
    for (size_t i = 3 * L; i < y.size(); i += L) o.push_back(y[i]);   // the first 3 L samples are the filter filling
    return o;
}

// windowed-sinc low-pass (Blackman), cut-off fc, evaluated only at every D-th output
std::vector<cf32> firDecimate(const std::vector<cf32>& y, double fs, double fc, double trans, size_t D) {
    int taps = (int)std::ceil(5.5 * fs / trans) | 1;
    taps = std::min(taps, 4001);
    std::vector<float> h(taps);
    double sum = 0;
    for (int i = 0; i < taps; i++) {
        const double m = i - (taps - 1) / 2.0;
        const double sinc = m == 0 ? 2 * fc / fs : std::sin(2 * kPi * fc / fs * m) / (kPi * m);
        const double w = 0.42 - 0.5 * std::cos(2 * kPi * i / (taps - 1)) + 0.08 * std::cos(4 * kPi * i / (taps - 1));
        h[i] = (float)(sinc * w);
        sum += h[i];
    }
    for (float& v : h) v = (float)(v / sum);
    std::vector<cf32> o;
    if (y.size() < (size_t)taps) return o;
    o.reserve((y.size() - taps) / D + 1);
    for (size_t i = 0; i + taps <= y.size(); i += D) {
        float re = 0, im = 0;
        for (int k = 0; k < taps; k++) { re += y[i + k].real() * h[k]; im += y[i + k].imag() * h[k]; }
        o.push_back(cf32(re, im));
    }
    return o;
}

struct Seg { double f = 0, w = 0; int first = 0, last = 0, n = 0; };

}  // namespace

const SubModeInfo* subModes(size_t& count) { count = sizeof kSub / sizeof *kSub; return kSub; }

Detection detect(const cf32* x, size_t n, double fs, const DetectOptions& o) {
    Detection d;
    if (!x || fs < 4000 || n < (size_t)(fs * 0.5)) { d.why = "too few samples (half a second at least)"; return d; }
    // ---- to the centre, filter, about 6 kHz
    std::vector<cf32> y(n);
    {
        const double w = -2 * kPi * o.centreHz / fs;
        double ph = 0;
        for (size_t i = 0; i < n; i++) {
            y[i] = x[i] * cf32((float)std::cos(ph), (float)std::sin(ph));
            ph += w;
            if (ph < -kPi) ph += 2 * kPi; else if (ph > kPi) ph -= 2 * kPi;
        }
    }
    double fs1 = fs;
    if (fs > 40000) {
        const size_t L = (size_t)std::floor(fs / 24000);
        y = boxDecimate(std::move(y), L);
        fs1 = fs / L;
    }
    const double hw = std::max(600.0, std::min(o.halfWidthHz, 2000.0));
    const size_t D2 = std::max<size_t>(1, (size_t)std::floor(fs1 / std::max(6000.0, 2.6 * hw)));
    y = firDecimate(y, fs1, hw + 50, 700, D2);
    const double fsd = fs1 / D2;
    // ---- short-time spectrum
    const int N = (int)std::lround(0.040 * fsd), hop = std::max(1, (int)std::lround(0.010 * fsd));
    int nfft = 1;
    while (nfft < 8 * N) nfft <<= 1;
    if ((int)y.size() < N + 10 * hop) { d.why = "too few samples after filtering"; return d; }
    const double binHz = fsd / nfft, hopSec = hop / fsd;
    const int kb = (int)std::floor(hw / binHz), kp = std::max(1, (int)std::lround(25.0 / binHz));
    std::vector<float> win(N);
    for (int i = 0; i < N; i++) win[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / N));
    Fft F(nfft);
    std::vector<cf32> b(nfft);
    std::vector<double> P(2 * kb + 1), Pm;
    const int frames = (int)((y.size() - N) / hop) + 1;
    std::vector<double> ff(frames, NAN), fw(frames, 0), purities;
    int clean = 0;
    for (int fr = 0; fr < frames; fr++) {
        std::fill(b.begin(), b.end(), cf32(0, 0));
        for (int i = 0; i < N; i++) b[i] = y[(size_t)fr * hop + i] * win[i];
        F.forward(b.data());
        double tot = 0, pk = -1; int kk = 0;
        for (int k = -kb; k <= kb; k++) {
            const double v = std::norm(b[(k + nfft) % nfft]);
            P[k + kb] = v; tot += v;
            if (v > pk) { pk = v; kk = k; }
        }
        if (tot <= 0) continue;
        // the noise per bin from the median (an exponential's median is ln 2 of its mean); the tone's share of the power above the noise
        Pm.assign(P.begin(), P.end());
        std::nth_element(Pm.begin(), Pm.begin() + Pm.size() / 2, Pm.end());
        const double med = Pm[Pm.size() / 2], nb = med / 0.6931;
        double pe = 0; int np = 0;
        for (int k = std::max(-kb, kk - kp); k <= std::min(kb, kk + kp); k++) { pe += P[k + kb]; np++; }
        const double sigTot = tot - nb * P.size(), sigPk = pe - nb * np;
        const double pur = sigTot > 0 ? std::min(1.0, sigPk / sigTot) : 0;
        if (pk < 12 * med || pur < 0.3 || kk <= -kb || kk >= kb) continue;
        const double a = std::log(P[kk - 1 + kb] + 1e-30), c0 = std::log(pk + 1e-30), c = std::log(P[kk + 1 + kb] + 1e-30);
        const double den = a - 2 * c0 + c;
        const double dk = den < 0 ? std::max(-0.5, std::min(0.5, 0.5 * (a - c) / den)) : 0;
        ff[fr] = (kk + dk) * binHz; fw[fr] = pe;
        purities.push_back(pur);
        clean++;
    }
    d.cleanShare = (double)clean / frames;
    d.purity = median(purities);
    if (clean < 10) { d.why = "no single clear tone in the frames"; return d; }
    d.valid = true;
    // ---- segments: frames on one tone; up to kGap noisy frames inside a segment do not split it
    constexpr int kGap = 3;
    std::vector<Seg> segs;
    Seg cur; bool open = false; int len = 0;
    auto close = [&]() { if (open && len >= 2) { cur.f /= cur.w; cur.n = len; segs.push_back(cur); } open = false; len = 0; };
    for (int fr = 0; fr < frames; fr++) {
        if (std::isnan(ff[fr])) { if (open && fr - cur.last > kGap) close(); continue; }
        if (open && std::fabs(ff[fr] - cur.f / cur.w) < 4.0) { cur.f += ff[fr] * fw[fr]; cur.w += fw[fr]; cur.last = fr; len++; continue; }
        close();
        cur = Seg(); cur.f = ff[fr] * fw[fr]; cur.w = fw[fr]; cur.first = cur.last = fr; open = true; len = 1;
    }
    close();
    {   // short segments are mostly noise peaks: when there are enough longer ones, only those count
        std::vector<Seg> lng;
        for (const Seg& s : segs) if (s.n >= 3) lng.push_back(s);
        if (lng.size() >= 8) segs.swap(lng);
    }
    d.segments = (int)segs.size();
    if (segs.size() < 4) { d.why = "too few tone segments"; return d; }
    // ---- symbol rate: the tone changes (one segment ending and another tone starting at most kGap frames later) fall on a grid of 1 / rate in
    // time; the lowest rate on whose grid they sit (by the vector strength of t * rate; every multiple of the rate fits as well)
    std::vector<double> tc;
    for (size_t i = 1; i < segs.size(); i++)
        if (segs[i].first - segs[i - 1].last <= 2 * kGap + 1 && std::fabs(segs[i].f - segs[i - 1].f) > 4.0) tc.push_back(0.5 * (segs[i].first + segs[i - 1].last) * hopSec);
    double rateR = 0;
    if (tc.size() >= 5) {
        const double tspan = tc.back() - tc.front();
        const double dr = 0.05 / (2 * kPi * std::max(1.0, tspan));
        std::vector<double> rr, rv;
        for (double r = 2.0; r <= 40.0; r += dr) {
            double re = 0, im = 0;
            for (double t : tc) { const double p = 2 * kPi * t * r; re += std::cos(p); im += std::sin(p); }
            rr.push_back(r); rv.push_back(std::hypot(re, im) / tc.size());
        }
        double vmax = 0;
        for (double v : rv) vmax = std::max(vmax, v);
        for (size_t i = 1; i + 1 < rv.size(); i++)
            if (rv[i] >= 0.85 * vmax && rv[i] >= rv[i - 1] && rv[i] >= rv[i + 1]) { d.baud = rr[i]; rateR = rv[i]; break; }
        if (rateR * std::sqrt((double)tc.size()) < 3.0) d.baud = 0;   // no grid in time stands out
    }
    // ---- tone grid: the largest spacing on which the segment frequencies sit
    std::vector<double> fq;
    for (const Seg& s : segs) fq.push_back(s.f);
    if (fq.size() > 200) fq.resize(200);
    const double ds = 0.004;
    std::vector<double> R;
    for (double s = 8.0; s <= 80.0; s += ds) {
        double re = 0, im = 0;
        for (double f : fq) { const double p = 2 * kPi * f / s; re += std::cos(p); im += std::sin(p); }
        R.push_back(std::hypot(re, im) / fq.size());
    }
    double rmax = 0;
    for (double r : R) rmax = std::max(rmax, r);
    size_t best = 0;
    for (size_t i = 1; i + 1 < R.size(); i++)
        if (R[i] >= 0.85 * rmax && R[i] >= R[i - 1] && R[i] >= R[i + 1]) best = i;
    d.spacingHz = 8.0 + best * ds;
    d.gridR = R[best];
    {
        double re = 0, im = 0;
        for (double f : fq) { const double p = 2 * kPi * f / d.spacingHz; re += std::cos(p); im += std::sin(p); }
        const double ref = d.spacingHz * std::atan2(im, re) / (2 * kPi);
        std::vector<long> idx;
        for (double f : fq) idx.push_back(std::lround((f - ref) / d.spacingHz));
        std::sort(idx.begin(), idx.end());
        d.distinctTones = (int)(std::unique(idx.begin(), idx.end()) - idx.begin());
    }
    std::vector<double> sf = fq;
    std::sort(sf.begin(), sf.end());
    const size_t cut = sf.size() >= 12 ? 1 : 0;   // one stray segment at either end does not set the span
    d.loHz = sf[cut]; d.hiHz = sf[sf.size() - 1 - cut];
    d.spanHz = d.hiHz - d.loHz;
    // ---- does it fit the reported ROS HF physical layer?
    const int S = d.segments;
    const double expSpan = 2234.0 * (S - 1) / (S + 1);   // the expected spread of S tones drawn from 144 on a 15.6 Hz grid
    const bool grid = d.spacingHz >= 12 && d.spacingHz <= 20 && d.gridR >= 0.4 && d.gridR * std::sqrt((double)S) >= 4.0;
    const bool span = d.spanHz >= 0.55 * expSpan && d.spanHz <= 2700;
    const bool single = d.purity >= 0.4;
    const bool many = d.distinctTones >= std::min(20, std::max(5, S / 3));
    const SubModeInfo* sub = nullptr;
    for (const SubModeInfo& m : kSub)
        if (m.identified && d.baud > 0 && std::fabs(d.baud / m.baud - 1) < 0.18) sub = &m;
    char t[256];
    snprintf(t, sizeof t, "%d tone segments, grid %.2f Hz (R %.2f), span %.0f Hz, %d grid positions, purity %.2f, rate %.2f Bd", S, d.spacingHz, d.gridR, d.spanHz,
             d.distinctTones, d.purity, d.baud);
    d.why = t;
    if (!grid) { d.why += "; no fine tone grid (12 to 20 Hz)"; return d; }
    if (!span) { d.why += "; not about 2.2 kHz wide"; return d; }
    if (!single) { d.why += "; more than one tone at a time"; return d; }
    if (!many) { d.why += "; too few different tones"; return d; }
    if (S < 6) { d.why += "; too few symbols"; return d; }
    d.ros = true;
    d.weak = S < 14 || !sub;
    if (sub) { d.subId = sub->id; d.name = sub->name; }
    else {
        snprintf(t, sizeof t, "ROS-like (rate %.1f Bd is not a reported sub-mode)", d.baud);
        d.name = t;
    }
    d.score = std::min(1.0, 0.45 * d.gridR + 0.25 * std::min(1.0, d.spanHz / 2000) + 0.15 * std::min(1.0, S / 20.0) + (sub ? 0.15 : 0));
    return d;
}

Detection detectAudio(const float* x, size_t n, double fs) {
    std::vector<cf32> c(n);
    for (size_t i = 0; i < n; i++) c[i] = cf32(x[i], 0);
    DetectOptions o;
    o.centreHz = 1550; o.halfWidthHz = 1400;
    Detection d = detect(c.data(), n, fs, o);
    if (d.valid) { d.loHz += o.centreHz; d.hiHz += o.centreHz; }
    return d;
}

std::vector<cf32> synthReportedGrid(double baud, double seconds, double fs, double centreHz, uint32_t seed, int tones, double spacingHz) {
    std::vector<cf32> o((size_t)(seconds * fs));
    std::mt19937 g(seed);
    std::uniform_int_distribution<int> pick(0, tones - 1);
    const double sps = fs / baud;
    double ph = 0, f = 0, next = 0;
    for (size_t i = 0; i < o.size(); i++) {
        if (i >= next) { f = centreHz + (pick(g) - (tones - 1) / 2.0) * spacingHz; next += sps; }
        o[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
        ph += 2 * kPi * f / fs;
        if (ph > kPi) ph -= 2 * kPi; else if (ph < -kPi) ph += 2 * kPi;
    }
    return o;
}

}  // namespace ros
}  // namespace dect2
