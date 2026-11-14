// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Robust Packet (RPR) physical layer (see rpr_dsp.h and docs/rpr.md).
//
// 1. The band: the average spectrum of the strongest frames is correlated with the comb template (+1 inside +-240 Hz, -1 in a guard band to +-360 Hz);
//    the best position is the comb centre (the edges are 12 Hz wide, so the centre is good to a few Hz).
// 2. Bursts: the eight carriers are mixed down and integrated over the active part of the symbol (1/60 s, which nulls the neighbours 60 Hz away); the summed power against the 20th percentile is the burst gate.
// 3. Each burst: the symbol clock from the 50 baud line of the summed envelope (two halves, so a clock error shows), the symbols of every carrier by
//    sampling at the clock, the differential phase d = s[m] conj(s[m-1]); the speed level is decided by the concentration of d^2 (D-BPSK) and d^4
//    (D-QPSK); the carrier frequency error shows as a constant turn of d and is removed per carrier (the phase step index is then relative to the
//    constellation axes: the phase convention of the real signal is not published).
#include "dect2/rpr/rpr_dsp.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace dect2 { namespace rpr {

namespace {
using dect2::math::kPi;
using cd = std::complex<double>;
constexpr int kSps = 160;   // samples per symbol at 8 kHz, 50 baud
constexpr int kWin = 133;   // the active part of an OFDM symbol, 1 / 60 s: the window that nulls the neighbouring carriers (60 Hz spacing)

void fft(std::vector<cd>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const cd w = std::polar(1.0, -2 * kPi / (double)len);
        for (size_t i = 0; i < n; i += len) {
            cd t(1, 0);
            for (size_t k = 0; k < len / 2; k++) {
                const cd u = a[i + k], v = a[i + k + len / 2] * t;
                a[i + k] = u + v; a[i + k + len / 2] = u - v; t *= w;
            }
        }
    }
}

// the comb centre from the spectrum: returns the template correlation contrast, centre in hz
bool findComb(const float* x, size_t n, double& centre, double& contrast) {
    const size_t N = 4096, H = 2048;
    if (n < N * 2) return false;
    const size_t F = (n - N) / H + 1;
    std::vector<double> win(N);
    for (size_t i = 0; i < N; i++) win[i] = 0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)N);
    const size_t b0 = (size_t)(900.0 / kFs * N), b1 = (size_t)(2100.0 / kFs * N);
    std::vector<std::vector<float>> frames;
    std::vector<double> fp;
    std::vector<cd> a(N);
    for (size_t f = 0; f < F; f++) {
        for (size_t i = 0; i < N; i++) a[i] = cd((double)x[f * H + i] * win[i], 0);
        fft(a);
        std::vector<float> p(b1 - b0 + 200);
        double tot = 0;
        for (size_t b = b0 - 100; b < b1 + 100; b++) { const double v = std::norm(a[b]); p[b - (b0 - 100)] = (float)v; if (b >= b0 && b < b1) tot += v; }
        frames.push_back(std::move(p)); fp.push_back(tot);
    }
    std::vector<double> sorted = fp;
    std::sort(sorted.begin(), sorted.end());
    const double thr = sorted[(size_t)((double)sorted.size() * 0.6)];
    std::vector<double> P(frames[0].size(), 0.0);
    size_t used = 0;
    for (size_t f = 0; f < frames.size(); f++) if (fp[f] >= thr) { for (size_t i = 0; i < P.size(); i++) P[i] += frames[f][i]; used++; }
    if (!used) return false;
    const double binHz = kFs / (double)N;
    auto at = [&](double hz) { const double b = hz / binHz - (double)(b0 - 100); const long i = (long)std::floor(b); if (i < 0 || i + 1 >= (long)P.size()) return 0.0; const double fr = b - (double)i; return P[(size_t)i] * (1 - fr) + P[(size_t)i + 1] * fr; };
    double best = -1e300, bestFc = 0, bestIn = 0, bestOut = 1;
    for (double fc = 1000; fc <= 2000; fc += 1.0) {
        double in = 0, out = 0; int ni = 0, no = 0;
        for (double d = -240; d <= 240; d += binHz) { in += at(fc + d); ni++; }
        for (double d = 252; d <= 372; d += binHz) { out += at(fc + d) + at(fc - d); no += 2; }
        const double sc = in / ni - out / no;
        if (sc > best) { best = sc; bestFc = fc; bestIn = in / ni; bestOut = out / no; }
    }
    centre = bestFc;
    contrast = bestIn / std::max(bestOut, 1e-30);
    return true;
}

}  // namespace

std::vector<double> carriersFor(double centreHz) {
    std::vector<double> v;
    for (int k = 0; k < kCarriers; k++) v.push_back(centreHz + (k - 3.5) * kSpacingHz);
    return v;
}

namespace {

// one active symbol part (1 / 60 s) integrated at every sample: out[i] = mean of the window centred on i of x * exp(-j 2 pi f t)
void symbolIntegrate(const float* x, size_t n, double f, std::vector<cd>& out) {
    std::vector<cd> z(n);
    const cd rot = std::polar(1.0, -2 * kPi * f / kFs);
    cd ph(1, 0);
    for (size_t i = 0; i < n; i++) { z[i] = ph * (double)x[i]; ph *= rot; if ((i & 1023) == 1023) ph /= std::abs(ph); }
    out.assign(n, cd(0, 0));
    cd acc(0, 0);
    for (size_t i = 0; i < n + (size_t)kWin / 2; i++) {
        if (i < n) acc += z[i];
        if (i >= (size_t)kWin) acc -= z[i - (size_t)kWin];
        const long o = (long)i - kWin / 2;
        if (i + 1 >= (size_t)kWin && o >= 0 && (size_t)o < n) out[(size_t)o] = acc / (double)kWin;
    }
}

cd interp(const std::vector<cd>& s, double t) {
    const long i = (long)std::floor(t);
    if (i < 0 || (size_t)i + 1 >= s.size()) return cd(0, 0);
    const double fr = t - (double)i;
    return s[(size_t)i] * (1 - fr) + s[(size_t)i + 1] * fr;
}

struct Seg { size_t a, b; };

bool demodBurst(const std::vector<std::vector<cd>>& S, const std::vector<double>& env, Seg g, Burst& out) {
    const size_t len = g.b - g.a;
    if (len < (size_t)kSps * 8) return false;
    // symbol clock: line of the envelope at 50 baud, whole burst and the two halves
    auto line = [&](size_t a, size_t b) {
        double mean = 0;
        for (size_t i = a; i < b; i++) mean += env[i];
        mean /= (double)(b - a);
        cd c(0, 0);
        for (size_t i = a; i < b; i++) c += (env[i] - mean) * std::polar(1.0, -2 * kPi * (double)i / (double)kSps);
        return c;
    };
    const cd c = line(g.a, g.b);
    double phase = -std::arg(c) / (2 * kPi) * kSps;     // position of the maximum of the envelope = symbol centres (mod 160)
    double ppm = 0;
    if (len > (size_t)kSps * 150) {   // a clock error only shows over seconds (a 1.4 s burst moves by a sample at 100 ppm)
        const size_t mid = g.a + len / 2;
        const double p1 = -std::arg(line(g.a, mid)) / (2 * kPi), p2 = -std::arg(line(mid, g.b)) / (2 * kPi);
        double d = p2 - p1; d -= std::round(d);
        ppm = d / ((double)(len / 2) / kSps) * 1e6;   // the peaks move later against the 160 sample grid: the symbols are longer
        ppm = std::max(-500.0, std::min(500.0, ppm));
    }
    const double sps = kSps * (1.0 + ppm * 1e-6);
    double t0 = (double)g.a;
    // first symbol centre at or after the start (the burst's ramp, kLead symbols, is skipped later by the gate edges)
    t0 = phase - std::floor((phase - t0) / kSps) * kSps;
    while (t0 < (double)g.a + 1) t0 += kSps;
    std::vector<double> t;
    for (double q = t0; q < (double)g.b - kSps / 2; q += sps) t.push_back(q);
    if (t.size() < 8) return false;
    const size_t K = t.size();
    std::vector<std::vector<cd>> dd(kCarriers, std::vector<cd>(K - 1));
    std::vector<double> r2(kCarriers), r4(kCarriers);
    for (int k = 0; k < kCarriers; k++) {
        cd prev = interp(S[(size_t)k], t[0]);
        cd a2(0, 0), a4(0, 0);
        for (size_t m = 1; m < K; m++) {
            const cd s = interp(S[(size_t)k], t[m]);
            cd d = s * std::conj(prev);
            const double ab = std::abs(d);
            d = ab > 1e-30 ? d / ab : cd(1, 0);
            dd[(size_t)k][m - 1] = d;
            a2 += d * d; a4 += d * d * d * d;
            prev = s;
        }
        r2[(size_t)k] = std::abs(a2) / (double)(K - 1);
        r4[(size_t)k] = std::abs(a4) / (double)(K - 1);
    }
    double m2 = 0, m4 = 0;
    for (int k = 0; k < kCarriers; k++) { m2 += r2[(size_t)k]; m4 += r4[(size_t)k]; }
    m2 /= kCarriers; m4 /= kCarriers;
    // D-BPSK: d = +-1 (r2 near 1); D-QPSK: d = the four axes (r2 near 0, r4 near 1)
    int order = 0; double r = 0;
    if (m2 > 0.4) { order = 2; r = m2; }
    else if (m4 > 0.25) { order = 4; r = m4; }
    out.order = order; out.level = order == 2 ? 1 : order == 4 ? 2 : 0; out.r = r;
    out.nSyms = (int)(K - 1);
    out.clockPpm = ppm;
    if (!order) return false;
    out.sym.assign(kCarriers, std::vector<uint8_t>(K - 1));
    double err2 = 0; size_t ne = 0;
    const double st = 2 * kPi / (double)order;
    for (int k = 0; k < kCarriers; k++) {
        cd a(0, 0);
        for (const cd& d : dd[(size_t)k]) a += order == 2 ? d * d : d * d * d * d;
        const double off = std::arg(a) / (double)order;   // frequency error and constellation offset of this carrier
        for (size_t m = 0; m < K - 1; m++) {
            double ang = std::arg(dd[(size_t)k][m] * std::polar(1.0, -off));
            double q = std::round(ang / st);
            const double e = ang - q * st;
            err2 += e * e; ne++;
            long qi = (long)q % order; if (qi < 0) qi += order;
            out.sym[(size_t)k][m] = (uint8_t)qi;
        }
    }
    out.carriers = 0;
    for (int k = 0; k < kCarriers; k++) if ((order == 2 ? r2[(size_t)k] : r4[(size_t)k]) >= 0.3) out.carriers++;
    out.snrDb = 10 * std::log10(1.0 / std::max(err2 / (double)ne, 1e-4));
    return true;
}

}  // namespace

Analysis analyse(const float* x, size_t n) {
    Analysis res;
    double fc = 0, contrast = 0;
    if (!findComb(x, n, fc, contrast)) { res.note = "too little audio"; return res; }
    if (contrast < 2.0) { res.note = "no eight-carrier comb"; return res; }
    res.centreHz = fc; res.combDb = 10 * std::log10(contrast);
    res.carrierHz = carriersFor(fc);
    std::vector<std::vector<cd>> S(kCarriers);
    for (int k = 0; k < kCarriers; k++) symbolIntegrate(x, n, res.carrierHz[(size_t)k], S[(size_t)k]);
    std::vector<double> env(n, 0.0);
    for (size_t i = 0; i < n; i++) { double e = 0; for (int k = 0; k < kCarriers; k++) e += std::norm(S[(size_t)k][i]); env[i] = e; }
    // gate: frames of 5 ms against the 20th percentile
    const size_t fr = 40, nf = n / fr;
    if (nf < 100) return res;
    std::vector<double> fe(nf), srt;
    for (size_t f = 0; f < nf; f++) { double a = 0; for (size_t i = 0; i < fr; i++) a += env[f * fr + i]; fe[f] = a / (double)fr; }
    srt = fe; std::sort(srt.begin(), srt.end());
    const double floorP = srt[nf / 5];
    const double thr = std::max(floorP * 3.0, srt[nf * 19 / 20] * 0.01);   // the second term: a noise-free recording has a floor of exactly 0
    std::vector<Seg> segs;
    size_t f = 0;
    while (f < nf) {
        if (fe[f] < thr) { f++; continue; }
        size_t a = f, b = f, gap = 0;
        while (f < nf) {
            if (fe[f] >= thr) { b = f; gap = 0; } else if (++gap > 12) break;   // 60 ms
            f++;
        }
        segs.push_back({a * fr, (b + 1) * fr});
    }
    for (const Seg& g : segs) {
        // trim the 3 symbol ramps of the pulse shape: the usable symbols start 1.5 symbols inside and end 1.5 inside
        Seg h{g.a + (size_t)(kSps * 3 / 2), g.b > (size_t)(kSps * 3 / 2) ? g.b - (size_t)(kSps * 3 / 2) : g.b};
        if (h.b <= h.a + (size_t)kSps * 8) continue;
        Burst bu;
        if (!demodBurst(S, env, h, bu) || bu.carriers < 5 || bu.nSyms < 20) continue;   // a real burst has most of its eight carriers and a preamble
        bu.startSec = (double)g.a / kFs; bu.lenSec = (double)(g.b - g.a) / kFs;
        res.bursts.push_back(std::move(bu));
    }
    for (const Burst& b : res.bursts) res.carriersSeen = std::max(res.carriersSeen, b.carriers);
    for (const Burst& b : res.bursts) res.snrDb = std::max(res.snrDb, b.snrDb);
    res.found = !res.bursts.empty();
    return res;
}

}} // namespace dect2::rpr
