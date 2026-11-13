// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal Lab stage 2: modulation family from envelope statistics, power-law spectral lines (x^n), the instantaneous-frequency
// histogram, a cyclic-prefix autocorrelation (OFDM) and the spectral flatness. Each family gets a score; the scores become confidences.
#include "dect2/signallab/signallab.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace dect2::signallab {

const char* modName(Mod m) {
    switch (m) {
        case Mod::OOK: return "OOK"; case Mod::ASK: return "ASK"; case Mod::FSK: return "FSK"; case Mod::MSK: return "MSK";
        case Mod::GMSK: return "GMSK"; case Mod::PSK: return "PSK"; case Mod::QAM: return "QAM"; case Mod::OFDM: return "OFDM";
        default: return "unknown";
    }
}

std::vector<float> kmeans1d(const std::vector<float>& v, int k, int iters) {
    std::vector<float> c(std::max(1, k), 0.f);
    if (v.empty() || k < 1) return c;
    std::vector<float> s = v;
    std::sort(s.begin(), s.end());
    for (int i = 0; i < k; i++) c[i] = s[std::min(s.size() - 1, (size_t)(((double)i + 0.5) / k * (double)s.size()))];
    std::vector<double> sum(k);
    std::vector<size_t> cnt(k);
    for (int it = 0; it < iters; it++) {
        std::fill(sum.begin(), sum.end(), 0.0); std::fill(cnt.begin(), cnt.end(), 0);
        // sorted values: boundaries are midpoints, so one sweep assigns them
        int j = 0;
        for (float x : s) {
            while (j + 1 < k && x > 0.5f * (c[j] + c[j + 1])) j++;
            sum[j] += x; cnt[j]++;
        }
        bool moved = false;
        for (int i = 0; i < k; i++) if (cnt[i]) { const float nc = (float)(sum[i] / (double)cnt[i]); moved |= nc != c[i]; c[i] = nc; }
        std::sort(c.begin(), c.end());
        if (!moved) break;
    }
    return c;
}

std::vector<float> instFreq(const std::vector<cf>& x, double fs) {
    std::vector<float> f(x.size(), 0.f);
    const double k = fs / (2 * M_PI);
    for (size_t i = 1; i < x.size(); i++) f[i] = (float)(std::arg(x[i] * std::conj(x[i - 1])) * k);
    if (f.size() > 1) f[0] = f[1];
    return f;
}

namespace {
std::vector<Burst> segsOf(const std::vector<cf>& x, const std::vector<Burst>* segs) {
    std::vector<Burst> s;
    if (segs && !segs->empty()) {
        for (const Burst& b : *segs) {   // trim the edges: ramps and the burst detector's smoothing
            const size_t m = (b.end - b.start) / 50;
            if (b.end > b.start + 2 * m + 16) s.push_back({b.start + m, b.end - m});
        }
    }
    if (s.empty()) s.push_back({0, x.size()});
    return s;
}

// accumulated |FFT(x^n)|^2 over the segments; returns the line strength (peak - mean) / std and where it is
double lineScore(const std::vector<cf>& x, const std::vector<Burst>& sg, int n, double fs, double* hz) {
    size_t mx = 0;
    for (const Burst& b : sg) mx = std::max(mx, b.end - b.start);
    const size_t N = std::clamp(nextPow2(mx), (size_t)1024, (size_t)65536);
    std::vector<double> acc(N, 0.0);
    std::vector<std::complex<double>> buf(N);
    size_t used = 0;
    for (const Burst& b : sg) {
        for (size_t s = b.start; s < b.end && used < (1u << 21); s += N) {
            const size_t e = std::min(b.end, s + N);
            if (e - s < 64) break;
            std::fill(buf.begin(), buf.end(), 0.0);
            for (size_t i = s; i < e; i++) {
                std::complex<double> z = x[i], p = 1;
                for (int k = 0; k < n; k++) p *= z;
                // high powers: keep the phase, weight by |z|^2 (the noise is not raised to the n-th power)
                if (n > 2) { const double a = std::abs(z); p = a > 0 ? p / std::pow(a, n - 2) : 0.0; }
                buf[i - s] = p;
            }
            fft(buf);
            for (size_t i = 0; i < N; i++) acc[i] += std::norm(buf[i]);
            used += e - s;
        }
    }
    double mean = 0, var = 0;
    for (double v : acc) mean += v;
    mean /= (double)N;
    for (double v : acc) var += (v - mean) * (v - mean);
    const double sd = std::sqrt(var / (double)N);
    size_t pk = (size_t)(std::max_element(acc.begin(), acc.end()) - acc.begin());
    double d = 0;
    if (pk > 0 && pk + 1 < N) {
        const double a = acc[pk - 1], b = acc[pk], c = acc[pk + 1];
        const double den = a - 2 * b + c;
        if (den != 0) d = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
    }
    double bin = (double)pk + d;
    if (bin >= (double)N / 2) bin -= (double)N;
    if (hz) *hz = bin * fs / (double)N;
    return sd > 0 ? (acc[pk] - mean) / sd : 0;
}

constexpr double kLine = 22;   // a power-law line stronger than this is real (noise peaks reach about ln(N) ~ 11)
constexpr double kLine8 = 14;  // x^8: the line is weaker (eight-fold noise growth even with the |z|^2 weighting), the bar lower
}

double powerLawOffset(const std::vector<cf>& x, double fs, int m, const std::vector<Burst>* segs) {
    double hz = 0;
    lineScore(x, segsOf(x, segs), std::max(1, m), fs, &hz);
    return hz / std::max(1, m);
}

ModFeatures modFeatures(const std::vector<cf>& x, double fs, double bwHz, const std::vector<Burst>* segs) {
    ModFeatures f;
    f.bwHz = bwHz;
    if (x.size() < 64) return f;
    const std::vector<Burst> sg = segsOf(x, segs);
    size_t tot = 0;
    for (const Burst& b : sg) tot += b.end - b.start;
    const size_t stride = std::max<size_t>(1, tot / 200000);
    // envelope
    std::vector<float> amp;
    for (const Burst& b : sg) for (size_t i = b.start; i < b.end; i += stride) amp.push_back(std::abs(x[i]));
    double m1 = 0, m2 = 0, m4 = 0;
    for (float a : amp) { m1 += a; m2 += (double)a * a; m4 += (double)a * a * a * a; }
    const double na = (double)amp.size();
    m1 /= na; m2 /= na; m4 /= na;
    f.envCv = m1 > 0 ? std::sqrt(std::max(0.0, m2 - m1 * m1)) / m1 : 0;
    f.envKurt = m2 > 0 ? m4 / (m2 * m2) : 0;
    const std::vector<float> lv = kmeans1d(amp, 2);
    f.ask2Ratio = lv[1] > 0 ? lv[0] / lv[1] : 0;
    size_t low = 0;
    for (float a : amp) if (a < 0.3f * lv[1]) low++;
    f.lowFrac = (double)low / na;
    // power-law lines
    for (int n : {1, 2, 4, 8}) f.line[n] = lineScore(x, sg, n, fs, &f.lineHz[n]);
    // instantaneous frequency, smoothed a little, where the amplitude is real
    const int L = std::max(1, (int)std::lround(0.75 * fs / std::max(1.0, bwHz)));
    std::vector<float> fr;
    const float gate = 0.4f * lv[1];
    for (const Burst& b : sg) {
        std::vector<cf> seg(x.begin() + (long)b.start, x.begin() + (long)b.end);
        const std::vector<float> fi = instFreq(seg, fs);
        double acc = 0;
        std::vector<float> sm(fi.size());
        for (size_t i = 0; i < fi.size(); i++) { acc += fi[i]; if (i >= (size_t)L) acc -= fi[i - L]; sm[i] = (float)(acc / std::min<double>((double)i + 1, L)); }
        for (size_t i = (size_t)L; i < sm.size(); i += stride) if (std::abs(seg[i]) > gate && std::abs(seg[i - L / 2]) > gate) fr.push_back(sm[i]);
    }
    if (fr.size() > 200) {
        double mean = 0;
        for (float v : fr) mean += v;
        mean /= (double)fr.size();
        double prevSep = 0;
        for (int k : {2, 4, 8}) {
            const std::vector<float> c = kmeans1d(fr, k);
            std::vector<size_t> cnt(k, 0);
            double W = 0;
            for (float v : fr) {
                int j = 0; float bd = std::fabs(v - c[0]);
                for (int i = 1; i < k; i++) if (std::fabs(v - c[i]) < bd) { bd = std::fabs(v - c[i]); j = i; }
                cnt[j]++; W += (double)bd * bd;
            }
            double B = 0;
            for (int i = 0; i < k; i++) B += (double)cnt[i] * (c[i] - mean) * (c[i] - mean);
            const double sep = W > 0 ? B / W : 0;
            f.freqSepK[k] = sep;
            size_t mn = fr.size();
            for (size_t v : cnt) mn = std::min(mn, v);
            double dm = 0, dv = 0;
            for (int i = 1; i < k; i++) dm += c[i] - c[i - 1];
            dm /= (k - 1);
            for (int i = 1; i < k; i++) dv += (c[i] - c[i - 1] - dm) * (c[i] - c[i - 1] - dm);
            const double unif = dm > 0 ? std::sqrt(dv / (k - 1)) / dm : 1;
            const double need = k == 2 ? 5.0 : k == 4 ? 9.0 : 16.0;
            // more levels only when each is well populated, evenly spaced and the split pays off (2-FSK transitions
            // otherwise pass for the inner levels of a 4-FSK)
            const bool better = k == 2 || (unif < 0.3 && sep > 2.2 * prevSep);
            if (sep >= need && (double)mn >= 0.5 / k * (double)fr.size() && better) {
                f.freqClusters = k; f.freqSep = sep;
                for (int i = 0; i < k; i++) f.freqLevels[i] = c[i];
            } else if (k > 2 && f.freqClusters == k / 2) break;
            prevSep = sep;
        }
    }
    // OFDM cyclic prefix: autocorrelation of the longest segment (FFT, circular, so only lags well below the block)
    {
        Burst lb = sg[0];
        for (const Burst& b : sg) if (b.end - b.start > lb.end - lb.start) lb = b;
        const size_t n = std::min<size_t>(lb.end - lb.start, 65536);
        const size_t N = nextPow2(2 * n);
        std::vector<std::complex<double>> b(N, 0.0);
        for (size_t i = 0; i < n; i++) b[i] = x[lb.start + i];
        fft(b);
        for (auto& v : b) v = std::norm(v);
        fft(b, true);
        const double r0 = std::abs(b[0]);
        const size_t lmin = std::max<size_t>(16, (size_t)(8 * fs / std::max(1.0, bwHz)));
        const size_t lmax = std::min<size_t>(8192, n / 4);
        for (size_t l = lmin; l < lmax; l++) {
            const double c = r0 > 0 ? std::abs(b[l]) / r0 * (double)n / (double)(n - l) : 0;
            if (c > f.cpCorr) { f.cpCorr = c; f.cpLag = l; }
        }
    }
    // flatness inside the band
    {
        std::vector<cf> act;
        for (const Burst& b : sg) { act.insert(act.end(), x.begin() + (long)b.start, x.begin() + (long)b.end); if (act.size() > 400000) break; }
        const std::vector<double> P = psd(act.data(), act.size(), 1024);
        const double bin = fs / 1024.0;
        double lg = 0, ar = 0; int c = 0;
        for (size_t i = 0; i < P.size(); i++) {
            const double hz = ((double)i - 512.0) * bin;
            if (std::fabs(hz) > 0.4 * bwHz) continue;
            lg += std::log(std::max(1e-30, P[i])); ar += P[i]; c++;
        }
        f.flatness = c && ar > 0 ? std::exp(lg / c) / (ar / c) : 0;
    }
    return f;
}

ModResult classify(const std::vector<cf>& x, double fs, double bwHz, const std::vector<Burst>* segs) {
    ModResult r;
    r.f = modFeatures(x, fs, bwHz, segs);
    const ModFeatures& f = r.f;
    std::vector<ModGuess> g;
    auto add = [&](Mod m, int order, double s, std::string why) { if (s > 0) g.push_back({m, order, s, std::move(why)}); };
    char buf[200];
    const bool constEnv = f.envCv < 0.25;
    // on-off keying: many samples near zero inside the bursts
    if (f.lowFrac > 0.12 && f.ask2Ratio < 0.3) {
        snprintf(buf, sizeof buf, "%.0f %% of the burst is near zero (two amplitude levels, low/high %.2f)", f.lowFrac * 100, f.ask2Ratio);
        add(Mod::OOK, 2, 0.55 + 0.4 * std::min(1.0, f.lowFrac / 0.35), buf);
    } else if (f.ask2Ratio > 0.2 && f.ask2Ratio < 0.75 && f.envCv > 0.2 && f.line[1] > kLine && std::fabs(f.lineHz[1]) < 0.05 * std::max(1.0, bwHz) && f.freqClusters == 0) {
        snprintf(buf, sizeof buf, "two amplitude levels (%.2f) and a carrier line", f.ask2Ratio);
        add(Mod::ASK, 2, 0.6, buf);
    }
    if (f.freqClusters >= 2) {
        snprintf(buf, sizeof buf, "%d instantaneous-frequency levels, separation %.1f, envelope variation %.2f", f.freqClusters, f.freqSep, f.envCv);
        double s = std::clamp(0.45 + f.freqSep / 60.0, 0.45, 0.95);
        if (!constEnv) s *= 0.5;
        if (f.lowFrac > 0.12) s *= 0.3;
        add(Mod::FSK, f.freqClusters, s, buf);
        // fewer levels stay candidates: Gaussian filtering (GMSK) splits two tones into several levels
        for (int k : {4, 2}) if (k < f.freqClusters) {
            snprintf(buf, sizeof buf, "%d instantaneous-frequency levels also fit (separation %.1f)", k, f.freqSepK[k]);
            add(Mod::FSK, k, s * (k == 2 ? 0.6 : 0.7), buf);
        }
        r.deviationHz = (f.freqLevels[f.freqClusters - 1] - f.freqLevels[0]) / 2;
        r.toneSpacingHz = (f.freqLevels[f.freqClusters - 1] - f.freqLevels[0]) / (f.freqClusters - 1);
    }
    // phase-shift keying: the x^M line where x^(M/2) has none
    const double ps = f.freqClusters >= 2 && constEnv ? 0.3 : f.lowFrac > 0.12 ? 0.3 : 1.0;
    auto pskScore = [&](int m) { return (0.5 + 0.45 * std::min(1.0, (f.line[m] - kLine) / (6 * kLine))) * ps; };
    if (f.line[2] > kLine) {
        snprintf(buf, sizeof buf, "x^2 line %.0f (carrier squared): two phases", f.line[2]);
        add(Mod::PSK, 2, pskScore(2), buf);
        // a two-phase preamble in front of QPSK / 8PSK data also squares to a line: keep the higher orders in the running
        if (f.line[4] > kLine) add(Mod::PSK, 4, pskScore(2) * 0.45, "x^4 line too: four phases with a two-phase preamble?");
        if (f.line[8] > kLine8) add(Mod::PSK, 8, pskScore(2) * 0.3, "x^8 line too: eight phases with a two-phase preamble?");
    } else if (f.line[4] > kLine) {
        snprintf(buf, sizeof buf, "x^4 line %.0f, no x^2 line: four phases (or QAM)", f.line[4]);
        const bool ringy = f.envCv > 0.42;
        add(Mod::PSK, 4, pskScore(4) * (ringy ? 0.6 : 1.0), buf);
        add(Mod::QAM, 16, pskScore(4) * (ringy ? 0.7 : 0.25), "x^4 line with a strongly varying envelope: QAM-like");
    } else if (f.line[8] > kLine8) {
        snprintf(buf, sizeof buf, "x^8 line %.0f, none at x^4: eight phases", f.line[8]);
        add(Mod::PSK, 8, ps * (0.5 + 0.45 * std::min(1.0, (f.line[8] - kLine8) / (6 * kLine8))), buf);
    }
    if (f.envKurt > 1.75 && f.cpCorr > 0.1 && f.line[2] < kLine && f.line[4] < kLine) {
        snprintf(buf, sizeof buf, "Gaussian-like envelope (kurtosis %.2f) with a cyclic-prefix echo %.2f at lag %zu", f.envKurt, f.cpCorr, f.cpLag);
        add(Mod::OFDM, 0, std::min(0.95, 0.5 + f.cpCorr * 2), buf);
    } else if (f.envKurt > 1.8 && f.flatness > 0.8 && f.line[2] < kLine && f.line[4] < kLine && f.freqClusters == 0) {
        add(Mod::OFDM, 0, 0.3, "noise-like envelope and a flat spectrum, but no cyclic prefix seen");
        r.spreadHint = true;
    }
    double sum = 0.25;
    for (const ModGuess& m : g) sum += m.confidence;
    for (ModGuess& m : g) m.confidence /= sum;
    std::sort(g.begin(), g.end(), [](const ModGuess& a, const ModGuess& b) { return a.confidence > b.confidence; });
    r.ranked = g;
    return r;
}

} // namespace dect2::signallab
