// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal Lab stage 3a: symbol rate. Four independent estimators and a vote:
//   cyclostationary : the line at the symbol rate in the spectrum of |x|^2 (linear modulations)
//   spectral line   : the line in the spectrum of the squared derivative of the symbol signal (transitions)
//   zero crossing   : the period that best fits all crossing instants (a comb fit, very precise on long segments)
//   autocorrelation : the spacing of the peaks in the autocorrelation of the transition indicator
#include "dect2/signallab/signallab.h"
#include <algorithm>
#include <cmath>

namespace dect2::signallab {

const char* rateMethodName(RateMethod m) {
    switch (m) {
        case RateMethod::Cyclostationary: return "cyclostationary";
        case RateMethod::SpectralLine: return "spectral line";
        case RateMethod::ZeroCrossing: return "zero crossing";
        case RateMethod::Autocorrelation: return "autocorrelation";
    }
    return "";
}

namespace {
double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
// accumulated power spectrum of real segments (mean removed), zero-padded x4; finds the strongest line in [fLo, fHi]
RateEstimate lineIn(const std::vector<std::vector<double>>& segs, double fs, RateMethod m) {
    RateEstimate r{m, 0, 0};
    size_t mx = 0;
    for (const auto& s : segs) mx = std::max(mx, s.size());
    if (mx < 32) return r;
    const size_t blk = std::min<size_t>(nextPow2(mx), 65536);
    const size_t N = blk * 4;
    std::vector<double> acc(N / 2, 0.0);
    std::vector<std::complex<double>> b(N);
    size_t used = 0;
    for (const auto& s : segs) {
        for (size_t st = 0; st < s.size() && used < (1u << 21); st += blk) {
            const size_t e = std::min(s.size(), st + blk);
            if (e - st < 32) break;
            double mean = 0;
            for (size_t i = st; i < e; i++) mean += s[i];
            mean /= (double)(e - st);
            std::fill(b.begin(), b.end(), 0.0);
            for (size_t i = st; i < e; i++) b[i - st] = s[i] - mean;
            fft(b);
            for (size_t i = 0; i < N / 2; i++) acc[i] += std::norm(b[i]);
            used += e - st;
        }
    }
    // search from 2 bins of the block resolution (well clear of DC) to fs / 2.2
    const size_t lo = std::max<size_t>(8, N / blk * 3), hi = (size_t)((double)N / 2.2);
    if (hi <= lo + 4) return r;
    // whiten: divide by a running median-ish (mean over a wide window) so a sloping continuum does not win
    std::vector<double> base(acc.size());
    const size_t W = std::max<size_t>(16, N / 256);
    {
        std::vector<double> cs(acc.size() + 1, 0.0);
        for (size_t i = 0; i < acc.size(); i++) cs[i + 1] = cs[i] + acc[i];
        for (size_t i = 0; i < acc.size(); i++) {
            const size_t a = i > W ? i - W : 0, e = std::min(acc.size(), i + W + 1);
            base[i] = (cs[e] - cs[a]) / (double)(e - a);
        }
    }
    size_t pk = lo;
    double best = 0;
    for (size_t i = lo; i < hi; i++) {
        const double v = base[i] > 0 ? acc[i] / base[i] : 0;
        if (v > best) { best = v; pk = i; }
    }
    double d = 0;
    if (pk > 0 && pk + 1 < acc.size()) {
        const double a = acc[pk - 1], c0 = acc[pk], c = acc[pk + 1], den = a - 2 * c0 + c;
        if (den != 0) d = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
    }
    r.baud = ((double)pk + d) * fs / (double)N;
    // a line is several times the local continuum; the zero-padding spreads it over ~4 bins
    r.strength = std::clamp((best - 3.0) / 30.0, 0.0, 1.0);
    return r;
}

std::vector<double> centreLevel(const std::vector<float>& s) {
    std::vector<double> o(s.begin(), s.end());
    if (o.empty()) return o;
    std::vector<float> c = kmeans1d(s, 2);
    const double mid = 0.5 * (c[0] + c[1]);
    for (auto& v : o) v -= mid;
    return o;
}
}

RateEstimate rateCyclo(const std::vector<std::vector<cf>>& segs, double fs) {
    std::vector<std::vector<double>> p;
    for (const auto& s : segs) { std::vector<double> q(s.size()); for (size_t i = 0; i < s.size(); i++) q[i] = std::norm(s[i]); p.push_back(std::move(q)); }
    return lineIn(p, fs, RateMethod::Cyclostationary);
}

RateEstimate rateSpectralLine(const std::vector<std::vector<float>>& segs, double fs) {
    std::vector<std::vector<double>> p;
    for (const auto& s : segs) {
        std::vector<double> q(s.size(), 0.0);
        for (size_t i = 1; i < s.size(); i++) { const double d = (double)s[i] - s[i - 1]; q[i] = d * d; }
        p.push_back(std::move(q));
    }
    return lineIn(p, fs, RateMethod::SpectralLine);
}

RateEstimate rateZeroCrossing(const std::vector<std::vector<float>>& segs, double fs) {
    RateEstimate r{RateMethod::ZeroCrossing, 0, 0};
    std::vector<std::vector<double>> times;   // crossing instants (samples) per segment
    std::vector<double> iv;
    size_t total = 0;
    for (const auto& s0 : segs) {
        const std::vector<double> s = centreLevel(s0);
        if (s.size() < 16) continue;
        double sd = 0;
        for (double v : s) sd += v * v;
        sd = std::sqrt(sd / (double)s.size());
        const double h = 0.25 * sd;
        std::vector<double> t;
        int state = 0;
        for (size_t i = 1; i < s.size(); i++) {
            const int ns = s[i] > h ? 1 : s[i] < -h ? -1 : state;
            if (state != 0 && ns != state) {
                // the actual zero crossing just before the trigger
                size_t j = i;
                while (j > 1 && (s[j] > 0) == (s[i] > 0)) j--;
                const double a = s[j], b = s[j + 1];
                t.push_back((double)j + (a != b ? a / (a - b) : 0.5));
            }
            state = ns;
        }
        for (size_t i = 1; i < t.size(); i++) iv.push_back(t[i] - t[i - 1]);
        total += t.size();
        times.push_back(std::move(t));
        if (total > 40000) break;
    }
    if (iv.size() < 8) return r;
    std::sort(iv.begin(), iv.end());
    // single-symbol runs are the commonest interval: the median of the shortest fifth (ignoring noise glitches)
    const size_t a = iv.size() / 50, e = std::max(a + 1, iv.size() / 5);
    double T0 = iv[(a + e) / 2];
    if (T0 < 1.5) T0 = 1.5;
    auto score = [&](double T) {
        double num = 0;
        size_t cnt = 0;
        for (const auto& t : times) {
            if (t.size() < 2) continue;
            double c = 0, s = 0;
            for (double v : t) { const double ph = 2 * M_PI * (v - t[0]) / T; c += std::cos(ph); s += std::sin(ph); }
            num += std::sqrt(c * c + s * s);
            cnt += t.size();
        }
        return cnt ? num / (double)cnt : 0.0;
    };
    double span = 0;
    for (const auto& t : times) if (!t.empty()) span = std::max(span, t.back() - t.front());
    // coarse grid over +-25 %, then finer around the best: the step shrinks with the segment length
    double best = T0, bs = -1;
    for (int i = -150; i <= 150; i++) { const double T = T0 * (1 + 0.25 * i / 150.0); if (T < 1.2) continue; const double v = score(T); if (v > bs) { bs = v; best = T; } }
    double step = T0 * 0.25 / 150.0;
    const double minStep = best * best / std::max(span, best) * 0.01;
    while (step > minStep) {
        step /= 4;
        double b2 = best;
        for (int i = -4; i <= 4; i++) { const double T = best + i * step; const double v = score(T); if (v > bs) { bs = v; b2 = T; } }
        best = b2;
    }
    // a sub-multiple of the true period fits equally well; take the largest that still fits (T, 2T, 3T)
    for (int k : {3, 2}) {
        const double v = score(best * k);
        if (v > 0.92 * clamp01(bs) && best * k < T0 * 1.6) { best *= k; bs = v; break; }
    }
    // the shortest interval need not be one symbol (Miller: 2, 3 and 4 half bits): half the period fitting clearly better wins
    if (score(best / 2) > bs + 0.1 && best / 2 >= 1.5) { best /= 2; bs = score(best); }
    r.baud = fs / best;
    r.strength = std::clamp(bs, 0.0, 1.0);
    return r;
}

RateEstimate rateAutocorr(const std::vector<std::vector<float>>& segs, double fs) {
    RateEstimate r{RateMethod::Autocorrelation, 0, 0};
    size_t mx = 0;
    for (const auto& s : segs) mx = std::max(mx, s.size());
    if (mx < 64) return r;
    const size_t blk = std::min<size_t>(nextPow2(mx), 32768), N = blk * 2;
    std::vector<double> R(N, 0.0);
    std::vector<std::complex<double>> b(N);
    for (const auto& s0 : segs) {
        const std::vector<double> s = centreLevel(s0);
        for (size_t st = 0; st + 32 < s.size(); st += blk) {
            const size_t e = std::min(s.size(), st + blk);
            std::fill(b.begin(), b.end(), 0.0);
            double mean = 0;
            for (size_t i = st + 1; i < e; i++) mean += ((s[i] > 0) != (s[i - 1] > 0)) ? 1.0 : 0.0;
            mean /= (double)(e - st);
            for (size_t i = st + 1; i < e; i++) b[i - st] = (((s[i] > 0) != (s[i - 1] > 0)) ? 1.0 : 0.0) - mean;
            fft(b);
            for (auto& v : b) v = std::norm(v);
            fft(b, true);
            for (size_t i = 0; i < N; i++) R[i] += b[i].real();
        }
    }
    // smooth over 3 lags (crossings jitter by a sample)
    const size_t maxLag = std::min<size_t>(blk / 4, 4096);
    std::vector<double> S(maxLag, 0.0);
    for (size_t l = 1; l + 1 < maxLag; l++) S[l] = R[l - 1] * 0.25 + R[l] * 0.5 + R[l + 1] * 0.25;
    double mxv = 0;
    for (size_t l = 2; l + 1 < maxLag; l++) mxv = std::max(mxv, S[l]);
    if (mxv <= 0) return r;
    // the first lag peak above 60 % of the highest one
    size_t T1 = 0;
    for (size_t l = 2; l + 1 < maxLag; l++) if (S[l] >= S[l - 1] && S[l] >= S[l + 1] && S[l] > 0.6 * mxv) { T1 = l; break; }
    if (!T1) return r;
    // refine on the k-th multiples: least-squares slope through the peak positions (parabolic)
    double sxy = 0, sxx = 0;
    for (int k = 1; k <= 12; k++) {
        const double c = k * (double)T1 * (sxx > 0 ? (sxy / sxx) / (double)T1 : 1.0);
        const size_t a = (size_t)std::max(2.0, c - T1 * 0.3), e = std::min(maxLag - 2, (size_t)(c + T1 * 0.3));
        if (e <= a) break;
        size_t p = a;
        for (size_t l = a; l <= e; l++) if (S[l] > S[p]) p = l;
        if (S[p] < 0.4 * mxv) break;
        const double y0 = S[p - 1], y1 = S[p], y2 = S[p + 1], den = y0 - 2 * y1 + y2;
        const double pos = (double)p + (den != 0 ? std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5) : 0.0);
        sxy += k * pos; sxx += (double)k * k;
    }
    const double T = sxx > 0 ? sxy / sxx : (double)T1;
    // contrast: peak over the level halfway between peaks
    const size_t half = std::min(maxLag - 1, (size_t)std::lround(T * 1.5));
    const double trough = std::max(0.0, S[half]);
    r.baud = fs / T;
    r.strength = std::clamp((S[T1] - trough) / std::max(1e-30, S[T1]), 0.0, 1.0);
    return r;
}

RateResult combineRates(const std::vector<RateEstimate>& m) {
    RateResult rr;
    rr.methods = m;
    double total = 0;
    for (const auto& e : m) if (e.baud > 0) total += e.strength;
    if (total <= 0) return rr;
    double bestW = -1, bestRate = 0;
    for (const auto& c : m) {
        if (c.baud <= 0 || c.strength <= 0) continue;
        double w = 0, sw = 0, swr = 0;
        for (const auto& e : m) {
            if (e.baud <= 0) continue;
            if (std::fabs(e.baud / c.baud - 1) < 0.02) {
                // the zero-crossing fit is the precise one when it agrees: weight it up for the value
                const double vw = e.strength * (e.method == RateMethod::ZeroCrossing ? 4 : 1);
                w += e.strength; sw += vw; swr += vw * e.baud;
            } else {
                // spectral lines often sit at a harmonic of the rate (each edge of a pulse counts): 2x and 3x lend support
                for (int k : {2, 3}) if (std::fabs(e.baud / (k * c.baud) - 1) < 0.02) w += 0.7 * e.strength;
            }
        }
        if (c.method == RateMethod::ZeroCrossing) w += 0.25 * c.strength;   // the tie-breaker: crossings measure the period itself
        if (w > bestW) { bestW = w; bestRate = swr / sw; }
    }
    rr.baud = bestRate;
    double mx = 0;
    for (const auto& e : m) if (e.baud > 0 && std::fabs(e.baud / bestRate - 1) < 0.02) mx = std::max(mx, e.strength);
    rr.confidence = std::clamp(std::min(1.0, bestW / total) * (0.5 + 0.5 * mx), 0.0, 1.0);
    return rr;
}

std::vector<float> symbolSignal(const std::vector<cf>& x, double fs, Mod m) {
    std::vector<float> s(x.size());
    if (m == Mod::FSK || m == Mod::MSK || m == Mod::GMSK) return instFreq(x, fs);
    for (size_t i = 0; i < x.size(); i++) s[i] = std::abs(x[i]);
    return s;
}

} // namespace dect2::signallab
