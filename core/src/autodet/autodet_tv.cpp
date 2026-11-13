// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TV auto-detection (autodet.h): which TV standard a channel carries and the parameters its receiver needs.
//
// 1. Spectrum: the occupied part (half-way between floor and plateau), its width and SNR; narrow lines (an ATSC pilot at the lower edge, the
//    vision and sound carriers of analogue TV with their spacing).
// 2. OFDM: the normalised autocorrelation |R(L)| / R(0) of the whole capture (one FFT) at the useful-symbol length of every OFDM mode of
//    every standard and channel width (and the cyclic PN headers of DTMB at 255 / 511 symbols), each within +-1500 ppm; a peak is measured
//    against the median of all lags (z). The best few are checked by folding x[n] x*[n+L] at the symbol period of each guard interval: the
//    true guard concentrates the product in one window of the period (the continual pilots of another mode at the same lag do not). The
//    occupied width must fit the standard's width at that channel raster.
// 3. DVB-T2 against DVB-T (2K / 4K / 8K): the P1 symbol, whose first and last parts repeat its middle at 1024 T, frequency shifted by
//    1 / (1024 T). 1K / 16K / 32K are DVB-T2 only.
// 4. A single carrier: the symbol rate from the line of |x|^2 at Rs (cyclostationarity), the roll-off from the width, then at the symbol
//    instants (matched filter, best of four phases per block) the fourth moment (1 PSK, 1.32 - 1.39 QAM, noise taken out), the 4th / 8th power
//    lines (QPSK / 8PSK) and the DVB-S2 start of frame (differential correlation, so a carrier offset does not matter).
#include "autodet_internal.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/dsp/resampler.h"
#include "dect2/dsp/rrc.h"
#include <algorithm>
#include <chrono>
#include <cstdio>

namespace dect2 {
namespace autodet {

using namespace detail;

namespace {

struct OfdmCand {
    int std;                   // TvDvbt (T or T2), TvDvbt2 (T2 only), TvIsdbt, TvAtsc3, TvDtmb
    double chanMhz;
    int fftK;
    double tuSec;              // lag (DTMB: the PN repetition)
    std::vector<double> guards;   // shares of tu (DTMB: unused)
    std::vector<const char*> guardNames;
    double bwShare;            // occupied width over the channel width
    int dtmbPn = 0; double dtmbPeriodSec = 0, dtmbWinSec = 0;
    // measured
    long lag = 0; double rho = 0, z = 0;
};

std::vector<OfdmCand> ofdmCandidates() {
    std::vector<OfdmCand> v;
    const std::vector<double> gT = {0.25, 0.125, 0.0625, 0.03125};
    const std::vector<const char*> gTn = {"1/4", "1/8", "1/16", "1/32"};
    const std::vector<double> gT2 = {0.25, 19.0 / 128, 0.125, 19.0 / 256, 0.0625, 0.03125, 1.0 / 128};
    const std::vector<const char*> gT2n = {"1/4", "19/128", "1/8", "19/256", "1/16", "1/32", "1/128"};
    for (double ch : {8.0, 7.0, 6.0, 5.0, 1.7}) {
        const double T = ch == 1.7 ? 71.0 / 131 * 1e-6 : 7.0 / (8 * ch) * 1e-6;
        for (int k : {1, 2, 4, 8, 16, 32}) {
            const bool t2only = k == 1 || k == 16 || k == 32 || ch == 1.7;
            OfdmCand c{t2only ? TvDvbt2 : TvDvbt, ch, k, 1024.0 * k * T, t2only ? gT2 : gT2, t2only ? gT2n : gT2n, k >= 16 ? 0.97 : 0.952};
            v.push_back(c);
        }
    }
    for (double ch : {6.0, 7.0, 8.0}) for (int m = 1; m <= 3; m++)
        v.push_back(OfdmCand{TvIsdbt, ch, 1 << m, 252e-6 * (1 << (m - 1)) * 6 / ch, gT, gTn, 0.929});
    struct A3 { double ch; int N; };
    for (A3 a : {A3{6, 0}, A3{6, 2}, A3{7, 5}, A3{8, 8}}) {   // fs = 0.384 (N + 16) MHz (A/322); N = 0 is the 6.144 MHz of the bootstrap
        const double fs = 0.384e6 * (a.N + 16);
        for (int k : {8, 16, 32}) {
            OfdmCand c{TvAtsc3, a.ch, k, 1024.0 * k / fs, {}, {}, 0.8438 * fs / (a.ch * 1e6)};
            static const int gi[] = {192, 384, 512, 768, 1024, 1536, 2048, 2432, 3072, 3648, 4096, 4864};
            static const char* const gin[] = {"GI 192", "GI 384", "GI 512", "GI 768", "GI 1024", "GI 1536", "GI 2048", "GI 2432", "GI 3072", "GI 3648", "GI 4096", "GI 4864"};
            for (int i = 0; i < 12; i++) if (gi[i] < 1024 * k / 2) { c.guards.push_back((double)gi[i] / (1024.0 * k)); c.guardNames.push_back(gin[i]); }
            v.push_back(c);
        }
    }
    for (double ch : {8.0, 6.0}) {
        const double rs = ch == 8 ? 7.56e6 : 5.67e6;
        OfdmCand a{TvDtmb, ch, 0, 255 / rs, {}, {}, 0.99}; a.dtmbPn = 420; a.dtmbPeriodSec = 4200 / rs; a.dtmbWinSec = 165 / rs; v.push_back(a);
        OfdmCand b{TvDtmb, ch, 0, 511 / rs, {}, {}, 0.99}; b.dtmbPn = 945; b.dtmbPeriodSec = 4725 / rs; b.dtmbWinSec = 434 / rs; v.push_back(b);
    }
    return v;
}

// fold x[n] x*[n+lag] at the period: max over the window position of |sum over win| against the median. A cyclic prefix sits in one window
// of the period (several times the median); a correlation spread over the whole symbol (continual pilots, repeated test data) does not.
double foldConcentration(const cf32* x, size_t n, long lag, double period, double win) {
    const long P = (long)std::floor(period);
    const long W = std::max(1L, (long)std::lround(win));
    if (P < 4 || W >= P || (size_t)lag + 16 >= n) return 0;
    std::vector<std::complex<double>> f((size_t)P, {0, 0});
    const size_t m = std::min(n - (size_t)lag, (size_t)1 << 20);
    for (size_t i = 0; i < m; i++) {
        const long b = (long)std::fmod((double)i, period);
        f[(size_t)std::min(b, P - 1)] += std::complex<double>(x[i]) * std::conj(std::complex<double>(x[i + (size_t)lag]));
    }
    std::complex<double> s(0, 0);
    for (long i = 0; i < W; i++) s += f[(size_t)i];
    double mx = 0;
    std::vector<double> a((size_t)P);
    for (long i = 0; i < P; i++) {
        a[(size_t)i] = std::abs(s);
        mx = std::max(mx, a[(size_t)i]);
        s += f[(size_t)((i + W) % P)] - f[(size_t)i];
    }
    const double med = quantile(a, 0.5);
    return med > 0 ? mx / med : 0;
}

// DVB-T2 P1: its part C (542 T) comes again 542 T later at the start of A, frequency shifted by fSH = 1 / (1024 T): peaks of
// |sum over 542 T of x[n] x*[n+542T] e^+-j2pi fSH n| over the median; returns the peaks found
int p1Peaks(const cf32* x, size_t n, double rate, double T, double& ratio) {
    ratio = 0;
    const long lag = std::lround(542 * T * rate);
    const long W = lag;
    if (W < 8 || (size_t)(lag + W) >= n) return 0;
    const double fsh = 1.0 / (1024 * T);
    int bestCount = 0;
    for (int sg : {1, -1}) {
        const size_t m = n - (size_t)lag;
        std::vector<std::complex<double>> c(m);
        const double w = sg * 2 * M_PI * fsh / rate;
        for (size_t i = 0; i < m; i++) {
            const double ph = w * (double)i;
            c[i] = std::complex<double>(x[i]) * std::conj(std::complex<double>(x[i + (size_t)lag])) * std::complex<double>(std::cos(ph), std::sin(ph));
        }
        std::vector<double> mag;
        mag.reserve(m / 4);
        std::complex<double> s(0, 0);
        for (long i = 0; i < W; i++) s += c[(size_t)i];
        std::vector<double> full(m - (size_t)W);
        for (size_t i = 0; i + (size_t)W < m; i++) {
            full[i] = std::abs(s);
            if ((i & 3) == 0) mag.push_back(full[i]);
            s += c[i + (size_t)W] - c[i];
        }
        const double med = quantile(mag, 0.5);
        if (med <= 0) continue;
        double mx = 0;
        int cnt = 0;
        long last = -10 * lag;
        for (size_t i = 0; i < full.size(); i++) {
            const double r = full[i] / med;
            mx = std::max(mx, r);
            if (r >= 8 && (long)i - last > 4 * lag) { cnt++; last = (long)i; }
        }
        if (mx > ratio) { ratio = mx; bestCount = cnt; }
    }
    return bestCount;
}

// narrow lines in the spectrum: bins more than excessDb over the median of the 129 bins around them
struct Line { double hz; double excessDb; };
std::vector<Line> lines(const std::vector<double>& psd, double rate, double excessDb) {
    std::vector<Line> out;
    const int N = (int)psd.size();
    const int h = 64;
    for (int k = h; k < N - h; k++) {
        if (!(psd[(size_t)k] >= psd[(size_t)k - 1] && psd[(size_t)k] >= psd[(size_t)k + 1])) continue;
        std::vector<double> a;
        for (int j = k - h; j <= k + h; j += 4) a.push_back(psd[(size_t)j]);
        const double med = quantile(a, 0.5);
        const double e = dB(psd[(size_t)k] / std::max(1e-30, med));
        if (e >= excessDb) out.push_back({(k - N / 2) * rate / N, e});
    }
    std::sort(out.begin(), out.end(), [](const Line& a, const Line& b) { return a.excessDb > b.excessDb; });
    return out;
}

struct SingleCarrier {
    double rs = 0, lineRatio = 0, kurt = 0, c4 = 0, c8 = 0;
    int sof = 0;
    std::string constellation;
};

// symbols at the matched filter's best phase (4 samples per symbol in, per block of 1024)
std::vector<cf32> symbolsOf(const std::vector<cf32>& z, double sps, double rolloff) {
    std::vector<cf32> sym;
    const int span = 8;
    const int taps = (int)(2 * span * sps) | 1;
    std::vector<float> h((size_t)taps);
    for (int i = 0; i < taps; i++) h[(size_t)i] = (float)dsp::rrc((i - (taps - 1) / 2.0) / sps, std::max(0.05, std::min(0.5, rolloff)));
    const size_t n = z.size();
    if (n < (size_t)taps * 4) return sym;
    std::vector<cf32> y(n - (size_t)taps);
    for (size_t i = 0; i < y.size(); i++) {
        cf32 a(0, 0);
        for (int j = 0; j < taps; j++) a += z[i + (size_t)j] * h[(size_t)j];
        y[i] = a;
    }
    const size_t nsym = (size_t)((double)y.size() / sps) - 2;
    const int ph = 8;
    for (size_t b0 = 0; b0 < nsym; b0 += 1024) {
        const size_t b1 = std::min(nsym, b0 + 1024);
        double best = -1; int bp = 0;
        for (int p = 0; p < ph; p++) {
            double e = 0;
            for (size_t k = b0; k < b1; k++) { const size_t i = (size_t)((double)k * sps + sps * p / ph); if (i < y.size()) e += std::norm(y[i]); }
            if (e > best) { best = e; bp = p; }
        }
        for (size_t k = b0; k < b1; k++) { const size_t i = (size_t)((double)k * sps + sps * bp / ph); if (i < y.size()) sym.push_back(y[i]); }
    }
    return sym;
}

// |the strongest line of s^m| over sum |s|^m, per block of 2048 symbols, averaged
double powerLine(const std::vector<cf32>& s, int m) {
    const int B = 2048;
    if (s.size() < (size_t)B) return 0;
    Fft f(B);
    std::vector<cf32> b((size_t)B);
    double acc = 0; int nb = 0;
    for (size_t b0 = 0; b0 + B <= s.size() && nb < 64; b0 += B, nb++) {
        double den = 0;
        for (int i = 0; i < B; i++) {
            const std::complex<double> v(s[b0 + (size_t)i]);
            std::complex<double> p = v;
            for (int k = 1; k < m; k++) p *= v;
            b[(size_t)i] = cf32((float)p.real(), (float)p.imag());
            den += std::abs(p);
        }
        f.forward(b.data());
        double mx = 0;
        for (int i = 0; i < B; i++) mx = std::max(mx, (double)std::abs(b[(size_t)i]));
        acc += den > 0 ? mx / den : 0;
    }
    return nb ? acc / nb : 0;
}

int s2SofCount(const std::vector<cf32>& s) {
    const uint32_t sof = 0x18D2E82;   // 26 bits
    int bits[26];
    for (int i = 0; i < 26; i++) bits[i] = (int)((sof >> (25 - i)) & 1);
    std::complex<double> r[26];
    for (int i = 1; i < 26; i++) {
        const double sg = (1 - 2 * bits[i]) * (1 - 2 * bits[i - 1]);
        r[i] = std::complex<double>(0, (i & 1) ? sg : -sg);
    }
    const size_t n = s.size();
    if (n < 64) return 0;
    std::vector<std::complex<double>> d(n);
    for (size_t i = 1; i < n; i++) d[i] = std::complex<double>(s[i]) * std::conj(std::complex<double>(s[i - 1]));
    int best = 0;
    for (int conjv = 0; conjv < 2; conjv++) {
        std::vector<long> at; long last = -100000;
        for (size_t k = 0; k + 26 < n; k++) {
            std::complex<double> c(0, 0); double e = 0;
            for (int i = 1; i < 26; i++) { const std::complex<double> v = conjv ? std::conj(d[k + (size_t)i]) : d[k + (size_t)i]; c += v * std::conj(r[i]); e += std::abs(v); }
            if (e > 0 && std::abs(c) / e > 0.8 && (long)k - last > 90) { at.push_back((long)k); last = (long)k; }
        }
        // a constant coding and modulation sends frames of one length: count the starts followed by two equal spacings
        int cnt = 0;
        for (size_t a = 0; a < at.size(); a++) {
            bool ok = false;
            for (size_t b2 = 0; b2 < at.size() && !ok; b2++) for (size_t c2 = 0; c2 < at.size() && !ok; c2++) {
                if (b2 == a || c2 == a || b2 == c2) continue;
                const long d1 = at[b2] - at[a], d2 = at[c2] - at[b2];
                ok = d1 > 0 && d2 > 0 && std::labs(d1 - d2) <= 2;
            }
            cnt += ok;
        }
        best = std::max(best, cnt);
    }
    return best;
}

SingleCarrier singleCarrier(const cf32* x, size_t n, double rate, const Occupied& oc) {
    SingleCarrier sc;
    double r1 = 0;
    // |z|^2 is twice as wide as z: keep the rate high (an aliased line is found where it folds to, below)
    const double want = std::min(rate, std::max(oc.bwHz * 2.3, 64e3));
    std::vector<cf32> z = toBaseband(x, std::min(n, (size_t)(rate * 0.25)), rate, oc.centreHz, oc.bwHz * 1.3, want, r1);
    if (z.size() < 4096 || r1 <= 0) return sc;
    // symbol rate: the line of |z|^2
    size_t N = 1;
    while (N * 2 <= z.size() && N < ((size_t)1 << 20)) N *= 2;
    std::vector<cf32> p(N);
    for (size_t i = 0; i < N; i++) p[i] = cf32(std::norm(z[i]), 0);
    double mean = 0;
    for (size_t i = 0; i < N; i++) mean += p[i].real();
    mean /= (double)N;
    for (size_t i = 0; i < N; i++) p[i] -= cf32((float)mean, 0);
    Fft f((int)N);
    f.forward(p.data());
    // the line against the local continuum (the |x|^2 spectrum falls with frequency): magnitude over the median of its block of bins
    const size_t B = 256;
    std::vector<double> mag(N / 2), bmed(N / 2 / B + 1, 0);
    for (size_t k = 0; k < N / 2; k++) mag[k] = std::abs(p[k]);
    for (size_t b0 = 0; b0 * B < N / 2; b0++) {
        std::vector<double> v(mag.begin() + (long)(b0 * B), mag.begin() + (long)std::min(N / 2, (b0 + 1) * B));
        bmed[b0] = quantile(v, 0.5);
    }
    auto white = [&](long k) { const double m = bmed[(size_t)k / B]; return m > 0 ? mag[(size_t)k] / m : 0; };
    auto alias = [&](double hz) { double a = std::fmod(hz, r1); if (a > r1 / 2) a = r1 - a; return a; };
    const double lo = oc.bwHz / 1.55, hi = oc.bwHz / 0.98;
    double rb = 0, rsBest = 0;
    for (double rs = lo; rs <= hi; rs += r1 / N) {
        const long k = (long)std::lround(alias(rs) / r1 * N);
        if (k < 8 || k >= (long)N / 2 - 1) continue;
        const double w = white(k);
        if (w > rb) { rb = w; rsBest = rs; }
    }
    sc.lineRatio = rb;
    if (rsBest > 0) {   // refine on the bins around the line
        const double fa = alias(rsBest);
        const long k = (long)std::lround(fa / r1 * N);
        const double a = mag[(size_t)k - 1], b = mag[(size_t)k], c = mag[(size_t)k + 1];
        const double den = a - 2 * b + c;
        const double d = den < 0 ? 0.5 * (a - c) / den : 0;
        const double faR = (k + d) * r1 / N;
        const double q = std::round(rsBest / r1), e = rsBest - q * r1;   // rs = q r1 + e, the line folds to |e|
        rsBest = q * r1 + (e < 0 ? -faR : faR);
    }
    sc.rs = sc.lineRatio >= 6 && rsBest > 0 ? rsBest : oc.bwHz / 1.2;   // no line: guess a 20 % roll-off
    // symbols at 4 or more samples per symbol: an integer interpolation, so the rate stays exact
    double r2 = 0;
    const size_t nUse = std::min(n, (size_t)(rate * std::min(0.2, 160000.0 / sc.rs)));
    std::vector<cf32> z1 = toBaseband(x, nUse, rate, oc.centreHz, oc.bwHz * 1.2, std::min(rate, std::max(sc.rs * 4, oc.bwHz * 1.3)), r2);
    if (r2 <= 0 || z1.size() < 4096) return sc;
    const int up = std::max(1, (int)std::ceil(4 * sc.rs / r2));
    std::vector<cf32> z4;
    if (up > 1) {
        RationalResampler rr;
        if (!rr.configure(r2, r2 * up)) return sc;
        rr.process(z1.data(), z1.size(), z4);
        r2 *= up;
    } else z4.swap(z1);
    if (z4.size() < 8192) return sc;
    const double ro = std::max(0.05, oc.bwHz / sc.rs - 1);
    std::vector<cf32> s = symbolsOf(z4, r2 / sc.rs, ro);
    if (s.size() < 4096) return sc;
    double m2 = 0, m4 = 0;
    for (const cf32& v : s) { const double a = std::norm(v); m2 += a; m4 += a * a; }
    m2 /= (double)s.size(); m4 /= (double)s.size();
    const double snr = std::pow(10.0, oc.snrDb / 10);
    const double Np = m2 / (1 + snr), S = m2 - Np;
    sc.kurt = S > 0 ? (m4 - 4 * S * Np - 2 * Np * Np) / (S * S) : 2;
    sc.c4 = powerLine(s, 4);
    sc.c8 = powerLine(s, 8);
    sc.sof = s2SofCount(s);
    if (sc.kurt < 1.16 && sc.c4 > 0.35) sc.constellation = "QPSK";
    else if (sc.kurt < 1.16 && sc.c8 > 0.25) sc.constellation = "8PSK";
    else if (sc.kurt < 1.25) sc.constellation = "PSK / APSK";
    else if (sc.kurt < 1.35) sc.constellation = "16-QAM";
    else sc.constellation = "64/256-QAM";
    return sc;
}

bool satelliteIf(double rf) { return (rf >= 940e6 && rf <= 2200e6) || rf >= 2.5e9; }

} // namespace

TvResult detectTv(const cf32* x, size_t n, double rate, const TvOptions& o) {
    const auto t0 = std::chrono::steady_clock::now();
    TvResult r;
    char b[256];
    auto done = [&]() { r.headline = tvSummary(r); r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); return r; };
    if (!x || n < 65536 || rate <= 0) { r.evidence.push_back("too few samples"); return done(); }
    const std::vector<double> psd = welch(x, n, 4096);
    const bool sat = o.satellite || satelliteIf(o.rfHz);
    // ---- analogue TV: the vision carrier and the sound carrier stand out as lines, even where the picture is in the noise
    const std::vector<Line> ln = lines(psd, rate, 14);
    if (!ln.empty() && !sat) {
        const Line& v = ln[0];
        static const double sp[] = {4.5e6, 5.5e6, 6.0e6, 6.5e6};
        static const char* const sys[] = {"M/N", "B/G", "I", "D/K, L"};
        for (int i = 0; i < 4 && r.std == TvUnknown; i++)
            for (size_t j = 1; j < ln.size() && j < 12; j++)
                if (std::fabs(std::fabs(ln[j].hz - v.hz) - sp[i]) < 60e3 && v.excessDb >= 22) {
                    r.std = TvAnalog; r.signal = true; r.p.atvSystem = sys[i]; r.p.chanMhz = i == 0 ? 6 : i == 1 ? 0 : 8;
                    r.p.centreHz = v.hz; r.verdict = VerdictSure; r.confidence = 0.95;
                    snprintf(b, sizeof b, "vision carrier %.0f dB over the picture at %+.2f MHz and a sound carrier %.1f MHz away: analogue %s", v.excessDb, v.hz / 1e6, sp[i] / 1e6, sys[i]);
                    r.evidence.push_back(b);
                    break;
                }
        if (r.std == TvAnalog) { r.cands.push_back({TvAnalog, 0.95, r.evidence.back()}); return done(); }
    }
    const Occupied oc = occupied(psd, rate, 4);
    if (!oc.ok) { r.evidence.push_back("no signal above the noise floor"); return done(); }
    r.signal = true;
    r.p.centreHz = oc.centreHz; r.p.bwHz = oc.bwHz; r.p.snrDb = oc.snrDb;
    snprintf(b, sizeof b, "a signal %.2f MHz wide at %+.2f MHz, %.0f dB over the floor, edges %.3f", oc.bwHz / 1e6, oc.centreHz / 1e6, oc.snrDb, oc.edge);
    r.evidence.push_back(b);

    // ---- OFDM and DTMB: the autocorrelation at the candidate lags
    size_t N = 1;
    while (N * 2 <= n && N < ((size_t)1 << 22)) N *= 2;
    std::vector<OfdmCand> cs = ofdmCandidates();
    long maxLag = 0;
    for (auto& c : cs) maxLag = std::max(maxLag, (long)(c.tuSec * rate * 1.002) + 2);
    std::vector<double> rho;
    if (maxLag < (long)N / 4) {
        std::vector<cf32> a(N);
        double p0 = 0;
        for (size_t i = 0; i < N; i++) { a[i] = x[i]; p0 += std::norm(x[i]); }
        Fft f((int)N);
        f.forward(a.data());
        for (auto& v : a) v = cf32(std::norm(v), 0);
        f.inverse(a.data());
        rho.resize((size_t)maxLag + 1);
        const double r0 = std::abs(a[0]) > 0 ? std::abs(a[0]) : 1;
        for (long L = 0; L <= maxLag; L++) rho[(size_t)L] = std::abs(a[(size_t)L]) / r0;
        (void)p0;
    }
    double sigma = 0;
    if (!rho.empty()) {
        std::vector<double> tail(rho.begin() + std::min<long>(64, maxLag), rho.end());
        sigma = quantile(tail, 0.5) * 1.25;
    }
    std::vector<OfdmCand*> found;
    if (sigma > 0) {
        for (auto& c : cs) {
            const double L0 = c.tuSec * rate;
            if (L0 < 32) continue;
            if (c.chanMhz * 1e6 * c.bwShare > rate * 1.02) continue;   // wider than the capture
            const long a0 = (long)std::floor(L0 * (1 - 0.0015)), a1 = (long)std::ceil(L0 * (1 + 0.0015)) + 1;
            for (long L = std::max(1L, a0); L <= std::min(maxLag, a1); L++) if (rho[(size_t)L] > c.rho) { c.rho = rho[(size_t)L]; c.lag = L; }
            c.z = c.rho / sigma;
            const double want = c.chanMhz * 1e6 * c.bwShare;
            if (c.z >= 6 && std::fabs(oc.bwHz - want) / want < 0.25) found.push_back(&c);
        }
    }
    // the occupied width must fit: within 8 % of the standard's width at this raster (a weak or tilted edge measures narrower)
    auto bwFit = [&](const OfdmCand& c) { const double want = c.chanMhz * 1e6 * c.bwShare; return std::fabs(oc.bwHz - want) / want; };
    double bestScore = 0;
    OfdmCand* best = nullptr;
    double bestK = 0, bestG = 0; const char* bestGn = "";
    std::sort(found.begin(), found.end(), [](const OfdmCand* a, const OfdmCand* b) { return a->z > b->z; });
    for (size_t i = 0; i < found.size() && i < 6; i++) {
        OfdmCand& c = *found[i];
        double K = 0, g = 0; const char* gn = "";
        if (c.std == TvDtmb) K = foldConcentration(x, n, c.lag, c.dtmbPeriodSec * rate, c.dtmbWinSec * rate);
        else for (size_t j = 0; j < c.guards.size(); j++) {
            const double kk = foldConcentration(x, n, c.lag, c.lag * (1 + c.guards[j]), c.lag * c.guards[j]);
            if (kk > K) { K = kk; g = c.guards[j]; gn = c.guardNames[j]; }
        }
        const double fit = bwFit(c);
        // OFDM has a brick-wall spectrum; a single carrier's roll-off makes soft edges (DTMB's 0.05 is in between)
        const double edgeMax = c.std == TvDtmb ? 0.065 : 0.03;
        double score = c.z * (K >= 3 ? 1.0 : K >= 2 ? 0.5 : 0.05) * (fit < 0.08 ? 1.0 : fit < 0.15 ? 0.5 : 0.15) * (oc.edge <= edgeMax || K >= 6 ? 1.0 : 0.05);   // a strong prefix fold needs no help (a real capture has neighbours at its edges)
        snprintf(b, sizeof b, "%s %s%s %.1f MHz: correlation %.3f at %.1f us (z %.0f), fold %.1f%s%s, width fit %.0f %%, edges %.3f", tvStdName(c.std),
                 c.std == TvDtmb ? "PN" : "", c.std == TvDtmb ? std::to_string(c.dtmbPn).c_str() : (std::to_string(c.fftK) + "K").c_str(), c.chanMhz,
                 c.rho, c.lag / rate * 1e6, c.z, K, gn[0] ? " at " : "", gn, fit * 100, oc.edge);
        r.evidence.push_back(b);
        if (score > bestScore) { bestScore = score; best = &c; bestK = K; bestG = g; bestGn = gn; }
    }
    if (best && bestScore >= 5) {
        OfdmCand& c = *best;
        r.p.chanMhz = c.chanMhz; r.p.fftK = c.fftK; r.p.tuUs = c.lag / rate * 1e6; r.p.cpCorr = c.rho; r.p.guard = bestG; r.p.guardName = bestGn;
        r.p.dtmbPn = c.dtmbPn;
        int std = c.std;
        if (std == TvDvbt) {   // T or T2: the P1 symbol
            const double T = c.chanMhz == 1.7 ? 71.0 / 131 * 1e-6 : 7.0 / (8 * c.chanMhz) * 1e-6;
            double ratio = 0;
            const int p1 = p1Peaks(x, n, rate, T, ratio);
            r.p.p1 = p1 > 0; r.p.p1Count = p1;
            snprintf(b, sizeof b, "P1 symbol: %d found (strongest %.0f x the median)", p1, ratio);
            r.evidence.push_back(b);
            std = p1 > 0 ? TvDvbt2 : TvDvbt;
            // a T2 guard that DVB-T does not have
            if (bestG > 0 && (std::fabs(bestG - 19.0 / 128) < 1e-6 || std::fabs(bestG - 19.0 / 256) < 1e-6 || std::fabs(bestG - 1.0 / 128) < 1e-6)) std = TvDvbt2;
        } else if (std == TvDvbt2) { double ratio = 0; const double T = c.chanMhz == 1.7 ? 71.0 / 131 * 1e-6 : 7.0 / (8 * c.chanMhz) * 1e-6; r.p.p1Count = p1Peaks(x, n, rate, T, ratio); r.p.p1 = r.p.p1Count > 0; }
        r.std = std;
        const bool strong = c.z >= 12 && bestK >= 3 && bwFit(c) < 0.08;
        r.verdict = strong ? VerdictSure : VerdictLikely;
        r.confidence = strong ? 0.97 : 0.8;
        if (std == TvDvbt && n < (size_t)(rate * 0.27)) { r.verdict = VerdictLikely; r.confidence = std::min(r.confidence, 0.8); }   // too short to rule P1 out
        // the runner-up of another standard close behind: unsure
        for (OfdmCand* q : found) {
            if (q == best || q->std == c.std || (q->std == TvDvbt2 && c.std == TvDvbt) || (q->std == TvDvbt && c.std == TvDvbt2)) continue;
            const double fq = bwFit(*q);
            const double sq = q->z * (fq < 0.08 ? 1.0 : fq < 0.15 ? 0.5 : 0.15);
            if (sq > bestScore * 0.8) { r.verdict = VerdictUnsure; r.confidence = 0.5; r.cands.push_back({q->std, 0.4, "close runner-up"}); }
        }
        r.cands.insert(r.cands.begin(), {std, r.confidence, "cyclic prefix / PN structure"});
        return done();
    }

    // ---- ATSC 1.0: the pilot at the lower edge of a 6 MHz signal
    if (!sat && oc.bwHz > 4.6e6 && oc.bwHz < 6.4e6) {
        for (const Line& l : ln) {
            if (l.excessDb < 14) break;
            if (std::fabs(l.hz - oc.loHz) < 0.45e6) {
                r.std = TvAtsc; r.p.atscPilot = true; r.p.chanMhz = 6;
                r.verdict = VerdictSure; r.confidence = 0.95;
                snprintf(b, sizeof b, "a pilot %.0f dB over the spectrum %.2f MHz above the lower edge of a %.2f MHz signal: ATSC 8-VSB", l.excessDb, (l.hz - oc.loHz) / 1e6, oc.bwHz / 1e6);
                r.evidence.push_back(b);
                r.cands.push_back({TvAtsc, 0.95, b});
                return done();
            }
        }
    }

    // ---- a single carrier: DVB-S / S2 or DVB-C
    const SingleCarrier sc = singleCarrier(x, n, rate, oc);
    r.p.symRate = sc.rs; r.p.rolloff = sc.rs > 0 ? std::max(0.0, oc.bwHz / sc.rs - 1) : 0; r.p.kurt = sc.kurt; r.p.constellation = sc.constellation; r.p.s2Sof = sc.sof;
    snprintf(b, sizeof b, "single carrier: symbol rate %.4f Msym/s (line %.0f x the median), roll-off %.2f, 4th moment %.2f, 4th / 8th power lines %.2f / %.2f, S2 start of frame %d",
             sc.rs / 1e6, sc.lineRatio, r.p.rolloff, sc.kurt, sc.c4, sc.c8, sc.sof);
    r.evidence.push_back(b);
    if (sc.rs <= 0) { r.verdict = VerdictUnsure; return done(); }
    if (sc.kurt > 1.6) {   // Gaussian at the symbol instants: not a single carrier (an OFDM signal not recognised, or noise)
        r.verdict = VerdictUnsure; r.confidence = 0.2;
        r.evidence.push_back("the samples look Gaussian at the symbol instants: not a single carrier, but no OFDM structure of a known standard either");
        return done();
    }
    const bool qam = sc.kurt >= 1.25;
    const bool psk = sc.kurt < 1.2 || sc.sof >= 3;
    const bool dtmbRate = std::fabs(sc.rs - 7.56e6) / 7.56e6 < 0.004 || std::fabs(sc.rs - 5.67e6) / 5.67e6 < 0.004;
    if (sc.sof >= 3) { r.std = TvDvbs; r.verdict = VerdictSure; r.confidence = 0.97; r.p.constellation += ", DVB-S2 frames"; }
    else if (dtmbRate && !sat && sc.lineRatio >= 6) { r.std = TvDtmb; r.verdict = VerdictLikely; r.confidence = 0.7; r.p.chanMhz = sc.rs > 7e6 ? 8 : 6; }
    else if (sat) { r.std = TvDvbs; r.verdict = qam ? VerdictUnsure : VerdictLikely; r.confidence = qam ? 0.5 : 0.85; }
    else if (qam) { r.std = TvDvbc; r.verdict = sc.lineRatio >= 6 ? VerdictLikely : VerdictUnsure; r.confidence = sc.lineRatio >= 6 ? 0.85 : 0.55; }
    else if (psk) { r.std = TvDvbs; r.verdict = sc.lineRatio >= 6 ? VerdictLikely : VerdictUnsure; r.confidence = sc.lineRatio >= 6 ? 0.8 : 0.5; }
    else { r.verdict = VerdictUnsure; r.confidence = 0.3; r.cands.push_back({TvDvbc, 0.3, "QAM or APSK, cannot tell"}); r.cands.push_back({TvDvbs, 0.3, ""}); }
    if (r.std != TvUnknown) r.cands.insert(r.cands.begin(), {r.std, r.confidence, "single carrier"});
    if (r.std == TvDvbc && !qam) r.verdict = VerdictUnsure;
    return done();
}

std::string tvSummary(const TvResult& r) {
    if (!r.signal) return "no TV signal found";
    if (r.std == TvUnknown) return "a signal, but the standard is unclear";
    char b[200];
    std::string s = tvStdName(r.std);
    const TvParams& p = r.p;
    if (r.std == TvDvbt || r.std == TvDvbt2 || r.std == TvAtsc3) {
        snprintf(b, sizeof b, ", %dK, %s%s, %g MHz", p.fftK, p.guard > 0 ? "GI " : "", p.guard > 0 ? p.guardName.c_str() : "guard ?", p.chanMhz);
        s += b;
        if (r.std == TvAtsc3) { const size_t q = s.find("GI GI"); if (q != std::string::npos) s.erase(q, 3); }
    } else if (r.std == TvIsdbt) { snprintf(b, sizeof b, ", mode %d, GI %s, %g MHz", p.fftK == 2 ? 1 : p.fftK == 4 ? 2 : 3, p.guard > 0 ? p.guardName.c_str() : "?", p.chanMhz); s += b; }
    else if (r.std == TvDtmb) { if (p.dtmbPn) { snprintf(b, sizeof b, ", PN%d", p.dtmbPn); s += b; } if (p.chanMhz > 0) { snprintf(b, sizeof b, ", %g MHz", p.chanMhz); s += b; } }
    else if (r.std == TvDvbs || r.std == TvDvbc) { snprintf(b, sizeof b, ", %.3f Msym/s, %s, roll-off %.2f", p.symRate / 1e6, p.constellation.empty() ? "?" : p.constellation.c_str(), p.rolloff); s += b; }
    else if (r.std == TvAnalog) { s += ", system " + p.atvSystem; }
    else if (r.std == TvAtsc) s += ", 8-VSB, 6 MHz";
    snprintf(b, sizeof b, " (%s, %.0f %%)", verdictName(r.verdict), r.confidence * 100);
    s += b;
    return s;
}

} // namespace autodet
} // namespace dect2
