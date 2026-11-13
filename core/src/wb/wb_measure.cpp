// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/wb/wb_measure.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include "dect2/common/mathconst.h"

namespace dect2 {
namespace wb {

namespace {
using dect2::math::kPi;

double lin(double db) { return std::pow(10.0, db / 10.0); }

double percentile(std::vector<float> v, double q) {
    if (v.empty()) return 0;
    const size_t k = std::min(v.size() - 1, (size_t)(q * (double)(v.size() - 1)));
    std::nth_element(v.begin(), v.begin() + (long)k, v.end());
    return v[k];
}

// the averaged spectrum of a time range: up to 256 segments spread over it, read straight from the source
void psdRange(const Source& s, uint64_t t0, uint64_t t1, int& N, int window, std::vector<float>& db, double& enbw) {
    const uint64_t len = t1 - t0;
    while (N > 64 && (uint64_t)N > len) N /= 2;
    const std::vector<float> w = makeWindow(window, N);
    double sw = 0, sw2 = 0;
    for (float v : w) { sw += v; sw2 += (double)v * v; }
    enbw = N * sw2 / (sw * sw);
    const bool real = s.real();
    std::vector<double> acc((size_t)N, 0.0);
    std::vector<cf32> buf((size_t)N);
    std::vector<float> re, im;
    size_t nseg = (size_t)std::min<uint64_t>(256, std::max<uint64_t>(1, (len / (uint64_t)N) * 2));
    int cnt = 0;
    for (size_t k = 0; k < nseg; k++) {
        const uint64_t st = nseg == 1 || len <= (uint64_t)N ? t0 : t0 + (len - (uint64_t)N) * k / (nseg - 1);
        std::fill(buf.begin(), buf.end(), cf32(0, 0));
        const size_t got = s.read(st, (size_t)N, buf.data());
        if (got < (size_t)N / 2) continue;
        for (int i = 0; i < N; i++) buf[(size_t)i] *= w[(size_t)i];
        fft(buf.data(), N, re, im);
        for (int i = 0; i < N; i++) acc[(size_t)i] += (double)std::norm(buf[(size_t)i]);
        cnt++;
    }
    if (!cnt) { db.clear(); return; }
    const double norm = 1.0 / ((double)cnt * sw * sw);
    if (real) {
        db.resize((size_t)N / 2 + 1);
        for (int i = 0; i <= N / 2; i++) db[(size_t)i] = (float)dbOf(acc[(size_t)i] * norm * 4);
    } else {
        db.resize((size_t)N);
        for (int i = 0; i < N; i++) db[(size_t)i] = (float)dbOf(acc[(size_t)((i + N / 2) % N)] * norm);
    }
}

} // namespace

std::vector<Peak> findPeaks(const std::vector<float>& d, double f0, double binHz, int topN, double floorDb, double minOver, int minSep) {
    std::vector<Peak> out;
    const int n = (int)d.size();
    struct C { int i; float v; };
    std::vector<C> c;
    for (int i = 1; i + 1 < n; i++)
        if (d[(size_t)i] >= d[(size_t)i - 1] && d[(size_t)i] > d[(size_t)i + 1] && d[(size_t)i] >= floorDb + minOver) c.push_back({i, d[(size_t)i]});
    std::sort(c.begin(), c.end(), [](const C& a, const C& b) { return a.v > b.v; });
    std::vector<int> taken;
    for (const C& k : c) {
        bool nearBy = false;
        for (int t : taken) if (std::abs(t - k.i) < minSep) { nearBy = true; break; }
        if (nearBy) continue;
        taken.push_back(k.i);
        // parabolic interpolation on the dB values
        const double a = d[(size_t)k.i - 1], b = d[(size_t)k.i], cc = d[(size_t)k.i + 1];
        const double den = a - 2 * b + cc;
        const double off = std::fabs(den) > 1e-9 ? std::max(-0.5, std::min(0.5, 0.5 * (a - cc) / den)) : 0;
        Peak p;
        p.freqHz = f0 + ((double)k.i + off) * binHz;
        p.levelDb = b - 0.25 * (a - cc) * off;
        // width at -3 dB: where the spectrum falls 3 dB on each side (interpolated)
        const double lvl = b - 3.0;
        double l = k.i, r = k.i;
        while (l > 0 && d[(size_t)l] > lvl) l--;
        while (r < n - 1 && d[(size_t)r] > lvl) r++;
        auto cross = [&](int in, int outi) {
            const double vi = d[(size_t)in], vo = d[(size_t)outi];
            return std::fabs(vi - vo) < 1e-9 ? (double)outi : in + (outi - in) * (vi - lvl) / (vi - vo);
        };
        const double lx = l < k.i ? cross((int)l + 1, (int)l) : (double)k.i;
        const double rx = r > k.i ? cross((int)r - 1, (int)r) : (double)k.i;
        p.widthHz = std::max(0.0, rx - lx) * binHz;
        out.push_back(p);
        if ((int)out.size() >= topN) break;
    }
    return out;
}

std::vector<Burst> detectBursts(const std::vector<float>& p, double step, double t0, double& thr, double& duty) {
    std::vector<Burst> out;
    duty = 1; thr = 0;
    if (p.size() < 8) return out;
    const double lo = percentile(p, 0.10), hi = percentile(p, 0.98);
    if (hi - lo < 6.0) { thr = lo; return out; }   // continuous (or nothing)
    thr = lo + 0.5 * (hi - lo);
    const double off = thr - 1.5;
    bool on = false;
    size_t st = 0, on_count = 0;
    auto close = [&](size_t end) {
        if (end - st < 2) return;
        Burst b;
        b.startSec = t0 + (double)st * step; b.lenSec = (double)(end - st) * step;
        double pk = -300, sum = 0;
        for (size_t i = st; i < end; i++) { pk = std::max(pk, (double)p[i]); sum += lin(p[i]); }
        b.peakDb = pk; b.meanDb = 10 * std::log10(sum / (double)(end - st));
        out.push_back(b);
    };
    for (size_t i = 0; i < p.size(); i++) {
        if (!on && p[i] > thr) { on = true; st = i; }
        else if (on && p[i] < off) { on = false; close(i); }
        if (on) on_count++;
    }
    if (on) close(p.size());
    // merge bursts split by a one-block dip
    std::vector<Burst> m;
    for (const Burst& b : out) {
        if (!m.empty() && b.startSec - (m.back().startSec + m.back().lenSec) <= 1.01 * step) { m.back().lenSec = b.startSec + b.lenSec - m.back().startSec; m.back().peakDb = std::max(m.back().peakDb, b.peakDb); }
        else m.push_back(b);
    }
    duty = (double)on_count / (double)p.size();
    return m;
}

bool estimateSymbolRate(const cf32* x, size_t n, double rate, SymEstimate& est) {
    est = SymEstimate();
    if (n < 4096 || rate <= 0) return false;
    n = std::min<size_t>(n, 1u << 20);
    // features
    std::vector<float> f1(n), f2(n);
    double m1 = 0;
    for (size_t i = 0; i < n; i++) { f1[i] = std::norm(x[i]); m1 += f1[i]; }
    m1 /= (double)n;
    for (float& v : f1) v = (float)(v - m1);
    std::vector<float> d(n, 0.f);
    for (size_t i = 1; i < n; i++) d[i] = (float)std::arg(x[i] * std::conj(x[i - 1]));
    // light smoothing of the discriminator against single-sample noise spikes
    std::vector<float> ds(n, 0.f);
    for (size_t i = 1; i + 1 < n; i++) ds[i] = (d[i - 1] + 2 * d[i] + d[i + 1]) * 0.25f;
    double m2 = 0;
    for (size_t i = 2; i < n; i++) { f2[i] = std::fabs(ds[i] - ds[i - 1]); m2 += f2[i]; }
    f2[0] = f2[1] = 0;
    m2 /= (double)n;
    for (float& v : f2) v = (float)(v - m2);
    // a third: |x|^2 steps (envelope transitions of ASK / PSK with sharp edges)
    std::vector<float> f3(n, 0.f);
    double m3 = 0;
    for (size_t i = 1; i < n; i++) { f3[i] = std::fabs(std::abs(x[i]) - std::abs(x[i - 1])); m3 += f3[i]; }
    m3 /= (double)n;
    for (float& v : f3) v = (float)(v - m3);
    int L = 1 << (int)std::floor(std::log2((double)n / 4));
    L = std::max(2048, std::min(L, 1 << 16));
    const std::vector<float> w = makeWindow(WinHann, L);
    struct Line { double f, strength; int feat; };
    std::vector<Line> lines;
    static const char* names[3] = {"envelope", "frequency steps", "envelope steps"};
    const std::vector<float>* feats[3] = {&f1, &f2, &f3};
    std::vector<float> re, im;
    for (int fi = 0; fi < 3; fi++) {
        std::vector<double> acc((size_t)L / 2, 0.0);
        std::vector<cf32> buf((size_t)L);
        int cnt = 0;
        const size_t step = (size_t)L / 2;
        for (size_t p = 0; p + (size_t)L <= n; p += step) {
            for (int i = 0; i < L; i++) buf[(size_t)i] = cf32((*feats[fi])[p + (size_t)i] * w[(size_t)i], 0);
            fft(buf.data(), L, re, im);
            for (int i = 0; i < L / 2; i++) acc[(size_t)i] += (double)std::norm(buf[(size_t)i]);
            cnt++;
        }
        if (!cnt) continue;
        // lines above their local median (a running median over +-W bins would be exact; a box mean of the lower values is enough)
        const int W = std::max(16, L / 128);
        std::vector<double> lg((size_t)L / 2);
        for (int i = 0; i < L / 2; i++) lg[(size_t)i] = std::log10(acc[(size_t)i] + 1e-30) * 10.0;
        const int lo = std::max(8, (int)(L * 8.0 / rate));       // at least 8 Hz
        const int hi = (int)(L / 2 * 0.5);                       // up to rate / 4
        std::vector<float> win;
        for (int i = lo; i < hi; i++) {
            if (!(acc[(size_t)i] >= acc[(size_t)i - 1] && acc[(size_t)i] > acc[(size_t)i + 1])) continue;
            win.clear();
            for (int j = std::max(1, i - W); j < std::min(L / 2, i + W); j++) if (std::abs(j - i) > 3) win.push_back((float)lg[(size_t)j]);
            if (win.size() < 8) continue;
            const double med = percentile(win, 0.5);
            const double str = lg[(size_t)i] - med;
            if (str < 6.0) continue;
            const double a = lg[(size_t)i - 1], b = lg[(size_t)i], c = lg[(size_t)i + 1];
            const double den = a - 2 * b + c;
            const double off = std::fabs(den) > 1e-9 ? std::max(-0.5, std::min(0.5, 0.5 * (a - c) / den)) : 0;
            lines.push_back({((double)i + off) * rate / L, str, fi});
        }
    }
    if (lines.empty()) return false;
    std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.strength > b.strength; });
    // the symbol rate is the lowest strong line of the best group: a line at half the rate that is nearly as strong wins
    Line best = lines.front();
    for (int k = 5; k >= 2; k--) {   // the strongest line may be a harmonic of the rate: a line at 1/k of it that is nearly as strong is the rate
        bool found = false;
        for (const Line& l : lines)
            if (std::fabs(l.f * k - best.f) < 0.03 * best.f && l.strength > best.strength - 10.0) { best = l; found = true; break; }
        if (found) break;
    }
    est.baud = best.f;
    est.strengthDb = best.strength;
    est.method = names[best.feat];
    for (const Line& l : lines) {
        bool dup = false;
        for (const auto& c : est.cands) if (std::fabs(c.first - l.f) < 0.02 * l.f) dup = true;
        if (!dup && est.cands.size() < 6) est.cands.emplace_back(l.f, l.strength);
    }
    return true;
}

bool measure(const Source& s, uint64_t t0, uint64_t t1, double f0, double f1, const MeasureOptions& o, Measurement& m,
             const std::function<bool(double)>& progress) {
    m = Measurement();
    t1 = std::min<uint64_t>(t1, s.length());
    if (t1 <= t0 + 64) { m.why = "select a longer stretch of time"; return false; }
    m.t0 = t0; m.t1 = t1; m.rate = s.rate(); m.real = s.real();
    const double fmin = s.real() ? 0 : -s.rate() / 2, fmax = s.real() ? s.rate() / 2 : s.rate() / 2;
    if (!(f1 > f0)) { f0 = fmin; f1 = fmax; }
    f0 = std::max(f0, fmin); f1 = std::min(f1, fmax);
    m.f0 = f0; m.f1 = f1;
    if (progress && !progress(0.02)) return false;
    // ---- spectrum, peaks, widths ----------------------------------------------------------------------------------------------
    int N = o.fftSize;
    double enbw = 1;
    psdRange(s, t0, t1, N, o.window, m.psd, enbw);
    if (m.psd.empty()) { m.why = "no samples in the selection"; return false; }
    m.binHz = s.rate() / N;
    m.psdF0 = s.real() ? 0 : -s.rate() / 2;
    const int nb = (int)m.psd.size();
    auto binOf = [&](double f) { return (int)std::lround((f - m.psdF0) / m.binHz); };
    const int k0 = std::max(0, binOf(f0)), k1 = std::min(nb - 1, std::max(k0 + 1, binOf(f1)));
    // noise floor: a low percentile of the whole spectrum (a flat noise floor dominates it), a little up for the segments' scatter
    {
        std::vector<float> v(m.psd.begin() + k0, m.psd.begin() + k1 + 1);
        m.noiseFloorDb = percentile(v, 0.2) + 0.8;
        m.peakDb = *std::max_element(v.begin(), v.end());
    }
    const std::vector<float> band(m.psd.begin() + k0, m.psd.begin() + k1 + 1);
    const double bandF0 = m.psdF0 + k0 * m.binHz;
    m.peaks = findPeaks(band, bandF0, m.binHz, o.topN, m.noiseFloorDb, o.minPeakDb);
    if (!m.peaks.empty()) m.carrierHz = m.peaks.front().freqHz;
    {   // smoothed spectrum for the widths
        const int sw = std::max(1, (int)band.size() / 400) | 1;
        std::vector<double> sm(band.size());
        for (size_t i = 0; i < band.size(); i++) {
            double a = 0; int c = 0;
            for (int j = -(sw / 2); j <= sw / 2; j++) { const long q = (long)i + j; if (q >= 0 && q < (long)band.size()) { a += lin(band[(size_t)q]); c++; } }
            sm[i] = 10 * std::log10(a / c + 1e-30);
        }
        const double pk = *std::max_element(sm.begin(), sm.end());
        auto width = [&](double down, bool& limited) {
            double thr = pk - down;
            limited = false;
            if (thr < m.noiseFloorDb + 3) { thr = m.noiseFloorDb + 3; limited = true; }
            long lo = -1, hi = -1;
            for (size_t i = 0; i < sm.size(); i++) if (sm[i] >= thr) { if (lo < 0) lo = (long)i; hi = (long)i; }
            return lo < 0 ? 0.0 : (double)(hi - lo + 1) * m.binHz;
        };
        bool lim = false;
        m.bw3Hz = width(3, lim);
        m.bw20Hz = width(20, m.bw20Limited);
        (void)lim;
    }
    {   // 99 % occupied bandwidth: the power above the noise floor, 0.5 % from each side
        const double nl = lin(m.noiseFloorDb);
        std::vector<double> e(band.size());
        double tot = 0, allp = 0;
        for (size_t i = 0; i < band.size(); i++) { allp += lin(band[i]); e[i] = std::max(0.0, lin(band[i]) - nl); tot += e[i]; }
        m.bandPowerDb = dbOf(allp / enbw);
        if (tot > 0) {
            double c = 0;
            long lo = 0, hi = (long)band.size() - 1;
            for (size_t i = 0; i < band.size(); i++) { c += e[i]; if (c >= 0.005 * tot) { lo = (long)i; break; } }
            c = 0;
            for (size_t i = band.size(); i-- > 0;) { c += e[i]; if (c >= 0.005 * tot) { hi = (long)i; break; } }
            m.obwLoHz = bandF0 + lo * m.binHz - m.binHz / 2;
            m.obwHiHz = bandF0 + hi * m.binHz + m.binHz / 2;
            m.obwHz = m.obwHiHz - m.obwLoHz;
            m.centreHz = 0.5 * (m.obwLoHz + m.obwHiHz);
            double sig = 0;
            for (long i = lo; i <= hi; i++) sig += lin(band[(size_t)i]);
            const double noise = nl * (double)(hi - lo + 1);
            m.snrDb = sig > noise ? 10 * std::log10((sig - noise) / noise) : 0;
        } else {
            m.centreHz = 0.5 * (f0 + f1); m.obwLoHz = f0; m.obwHiHz = f1; m.obwHz = f1 - f0; m.snrDb = 0;
        }
        m.carrierOffsetHz = m.carrierHz - m.centreHz;
    }
    if (progress && !progress(0.2)) return false;
    // ---- the band as a baseband: power over time, bursts, symbol rate, identification ---------------------------------------------
    const double bw = std::max(m.obwHz * 1.4, 4 * m.binHz);
    double centre = m.centreHz;
    const double bwSel = std::min(bw, (f1 - f0));
    // the extraction needs the whole band when the user chose one that is narrower than the occupied band; otherwise the occupied band
    const bool userBand = (f0 > fmin + 1e-9 || f1 < fmax - 1e-9);
    const double exBw = userBand ? (f1 - f0) : bwSel;
    if (userBand) centre = 0.5 * (f0 + f1);
    if (s.real() && centre - exBw / 2 < 0) centre = exBw / 2;
    Extracted e;
    const double wanted = std::max(exBw * 2.5, 2000.0);
    auto prog = [&](double v) { return !progress || progress(0.2 + 0.4 * v); };
    // at most about 6 M source samples of the range feed the baseband (long selections are read in full only when they fit)
    const uint64_t maxSrc = 64u << 20;
    if (!extractBand(s, t0, std::min<uint64_t>(t1, t0 + maxSrc), centre, exBw, std::min(wanted, s.rate()), e, 1u << 23, prog) || e.x.size() < 64) {
        m.valid = true;   // the spectrum is there
        return true;
    }
    m.valid = true;
    if (o.bursts) {
        const double dur = (double)e.x.size() / e.rate;
        const double step = std::max(dur / 4000.0, 16.0 / e.rate);
        const size_t blk = std::max<size_t>(1, (size_t)std::lround(step * e.rate));
        m.powerStepSec = (double)blk / e.rate;
        m.powerT0Sec = (double)t0 / s.rate();
        for (size_t i = 0; i + blk <= e.x.size(); i += blk) {
            double a = 0;
            for (size_t k = 0; k < blk; k++) a += (double)std::norm(e.x[i + k]);
            m.power.push_back((float)dbOf(a / (double)blk));
        }
        m.bursts = detectBursts(m.power, m.powerStepSec, m.powerT0Sec, m.burstThresholdDb, m.duty);
    }
    if (progress && !progress(0.7)) return false;
    // the symbol rate and the modulation guess run on the signal while it is on
    std::vector<cf32> sig;
    if (!m.bursts.empty()) {
        const double r = e.rate;
        for (const Burst& b : m.bursts) {
            const size_t a = (size_t)std::max(0.0, (b.startSec - m.powerT0Sec) * r), z = std::min(e.x.size(), (size_t)((b.startSec + b.lenSec - m.powerT0Sec) * r));
            if (z > a) sig.insert(sig.end(), e.x.begin() + (long)a, e.x.begin() + (long)z);
            if (sig.size() > (1u << 20)) break;
        }
    }
    const std::vector<cf32>& use = sig.size() >= 4096 ? sig : e.x;
    if (o.symbolRate) estimateSymbolRate(use.data(), use.size(), e.rate, m.sym);
    if (progress && !progress(0.85)) return false;
    if (o.ident && use.size() >= 8192) {
        sigid::Options so;
        so.rfHz = s.centerHz() + centre;
        m.ident = sigid::identify(use.data(), std::min<size_t>(use.size(), 1u << 20), e.rate, so);
        m.identValid = m.ident.f.valid;
    }
    if (progress) progress(1.0);
    return true;
}

std::string peaksCsv(const Measurement& m, double centerHz) {
    std::string s = "rank,frequency_hz,absolute_frequency_hz,level_dbfs,width_3db_hz\n";
    char b[200];
    int i = 1;
    for (const Peak& p : m.peaks) {
        snprintf(b, sizeof b, "%d,%.3f,%.3f,%.2f,%.3f\n", i++, p.freqHz, centerHz + p.freqHz, p.levelDb, p.widthHz);
        s += b;
    }
    return s;
}

} // namespace wb
} // namespace dect2
