// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL streaming voice receiver. Port of kaal_rx.py (batch) to a chunked pipeline; see kaal_rx.h for the overview.
#include "dect2/kaal/kaal_rx.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/dsp/resampler.h"
#include "dect2/kaal/kaal_codec3.h"
#include "dect2/kaal/kaal_fec.h"
#include "dect2/kaal/kaal_phy.h"
#include "dect2/kaal/kaal_sweepid.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <map>
#include <vector>

namespace dect2::kaal {
namespace {

using cd = std::complex<double>;
const double kPi = 3.14159265358979323846;
constexpr size_t kAcqLen = 3 * (size_t)kFs;             // longest acquisition buffer (the old fixed length)
// Acquisition is tried as soon as the buffer can hold two cleaned sweep periods (1.01 s; the estimates need that much cleaned signal: with
// one period, 0.5 s, the lock was persistently worse), then on a growing buffer; a weak signal that fails the short tries is still found on the 3 s one, as before.
constexpr size_t kAcqTry[] = {60000, 66000, 72000, 84000, kAcqLen};
constexpr size_t kAcqN = sizeof(kAcqTry) / sizeof(kAcqTry[0]);
constexpr size_t kLongTry = 3;                          // kAcqTry entries below this are the short (under one second) buffers
constexpr int kAlignFrames = 27;                        // frames gathered to find the block offset (4 offsets, a wrong one decodes nothing: 12 rows are plenty)
constexpr int kAlignRows = 24;
constexpr int kMaxPendingWeak = 3;

double pymod(double x, double m) { return x - m * std::floor(x / m); }

struct Layout {
    std::vector<int> pilotIdx, dataIdx;
    std::vector<cd> pre;
    cd pilot;
    Layout() {
        for (int g = 0; g < kRtNPilots; g++) pilotIdx.push_back(kPreambleLen + kPilotEvery * g + (kPilotEvery - 1));
        std::vector<char> isP(kRtFrameSymbols, 0);
        for (int p : pilotIdx) isP[p] = 1;
        for (int i = kPreambleLen; i < kRtFrameSymbols; i++) if (!isP[i]) dataIdx.push_back(i);
        for (const cf& c : preamble()) pre.emplace_back(c.real(), c.imag());
        const cf p = pilotSymbol();
        pilot = cd(p.real(), p.imag());
    }
};
const Layout& layout() { static const Layout l; return l; }

// ---------------------------------------------------------------- small numeric helpers
// np.unwrap: corrections accumulate over the series
std::vector<double> unwrapped(const std::vector<double>& in) {
    std::vector<double> out(in.size());
    double off = 0;
    for (size_t i = 0; i < in.size(); i++) {
        if (i > 0) {
            const double d = in[i] - in[i - 1];
            const double w = d - 2 * kPi * std::floor((d + kPi) / (2 * kPi));
            off += w - d;
        }
        out[i] = in[i] + off;
    }
    return out;
}
void polyfit1(const std::vector<double>& x, const std::vector<double>& y, double& slope, double& icpt) {
    const size_t n = x.size();
    double mx = 0, my = 0;
    for (size_t i = 0; i < n; i++) { mx += x[i]; my += y[i]; }
    mx /= n; my /= n;
    double sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; i++) { sxx += (x[i] - mx) * (x[i] - mx); sxy += (x[i] - mx) * (y[i] - my); }
    slope = sxx > 0 ? sxy / sxx : 0;
    icpt = my - slope * mx;
}
double parabola(double a, double b, double c) {
    const double den = a - 2 * b + c;
    return den < 0 ? std::min(0.5, std::max(-0.5, 0.5 * (a - c) / den)) : 0.0;
}
double sincd(double u) { return std::fabs(u) < 1e-12 ? 1.0 : std::sin(kPi * u) / (kPi * u); }

// 16-tap windowed-sinc interpolation of buf (whose first element has absolute index base) at the float position t; zero outside.
cd interpAt(const std::vector<cd>& buf, int64_t base, double t) {
    const int64_t i0 = (int64_t)std::floor(t);
    const double f = t - (double)i0;
    if (i0 - 7 < base || i0 + 8 >= base + (int64_t)buf.size()) return cd(0, 0);
    cd acc(0, 0);
    for (int k = -7; k <= 8; k++) {
        const double u = k - f;
        const double h = sincd(u) * (0.5 + 0.5 * std::cos(kPi * u / 8.5));
        acc += buf[(size_t)(i0 + k - base)] * h;
    }
    return acc;
}

// ---------------------------------------------------------------- sweep
// The transmitted sweep has one of four variants per period of kSwL samples (kaal_sweepid.h). With a carrier offset f the correlation
// peak of variant v sits at t + u_v * d, d = -f * kSwShiftPerHz, where t is the start of the period. A variant that is shifted in time by
// s samples is the formula of the variant at tau = (n - s) / Fs, continued analytically outside [0, P): that is also the carrier-offset
// signal, so a shifted template with a complex gain removes the sweep of the period exactly, whatever the offset.
constexpr int kSwWin = 5;       // peak search window (+-) around a tracked variant position
constexpr int kSwWinNew = 40;   // ... for a variant not yet seen at the tracked position

const std::array<std::vector<cd>, 4>& sweepTemplatesD() {
    static const std::array<std::vector<cd>, 4> t = [] {
        std::array<std::vector<cd>, 4> a;
        for (int v = 0; v < 4; v++) {
            const auto& f = sweepVariantTemplate(v);
            a[(size_t)v].resize(f.size());
            for (size_t i = 0; i < f.size(); i++) a[(size_t)v][i] = cd(f[i].real(), f[i].imag());
        }
        return a;
    }();
    return t;
}

// What the batch search finds in the acquisition buffer.
struct SweepSeed {
    double t0 = 0;        // start of a period (absolute sample index), with the variant offsets of dshift
    double per = kSwL;    // period length in samples
    double dshift = 0;    // d = -cfo * kSwShiftPerHz
    int refClass = 0;     // the variant the seed was measured on (its peak position t0 + u * dshift is the measured one)
    int classes = 0;      // number of variants seen in the acquisition buffer
    bool dExact = false;  // dshift was fitted from the peaks of variants with different shifts (not just the coarse search grid)
    bool ok = false;
};

// Re-references a seed to another d: the measured peak of refClass stays where it is.
SweepSeed reseed(const SweepSeed& s, double dNew) {
    SweepSeed r = s;
    const double sRef = s.t0 + kSwU[s.refClass] * s.dshift;
    r.dshift = dNew;
    r.t0 = sRef - kSwU[s.refClass] * dNew;
    return r;
}

// Batch search over the acquisition buffer x (DC removed): FFT correlation with the four variants, fold modulo the period, joint search
// over the carrier-offset shift d, then a line fit of the winning peak positions for the period length.
bool sweepSearch(const std::vector<cd>& x, SweepSeed& seed) {
    const int64_t N = (int64_t)x.size(), L = kSwL;
    if (N < L + 1000) return false;
    int nfft = 1;
    while (nfft < N + L) nfft <<= 1;
    Fft fft(nfft);
    std::vector<cf32> A((size_t)nfft, cf32(0, 0)), Bv((size_t)nfft);
    for (int64_t i = 0; i < N; i++) A[(size_t)i] = cf32((float)x[(size_t)i].real(), (float)x[(size_t)i].imag());
    fft.forward(A.data());
    const int64_t nm = N - L + 1;
    std::vector<std::vector<float>> mag2(4, std::vector<float>((size_t)nm));
    std::vector<std::vector<double>> fold(4, std::vector<double>((size_t)L, 0.0));
    std::vector<double> cnt((size_t)L, 0.0);
    for (int v = 0; v < 4; v++) {
        std::fill(Bv.begin(), Bv.end(), cf32(0, 0));
        const auto& T = sweepVariantTemplate(v);
        for (int64_t i = 0; i < L; i++) Bv[(size_t)i] = cf32(T[(size_t)i].real(), T[(size_t)i].imag());
        fft.forward(Bv.data());
        std::vector<cf32> C((size_t)nfft);
        for (int i = 0; i < nfft; i++) C[(size_t)i] = A[(size_t)i] * std::conj(Bv[(size_t)i]);
        fft.inverse(C.data());
        for (int64_t i = 0; i < nm; i++) {
            const double m = std::norm(cd(C[(size_t)i].real(), C[(size_t)i].imag()));
            mag2[(size_t)v][(size_t)i] = (float)m;
            fold[(size_t)v][(size_t)(i % L)] += m;
            if (v == 0) cnt[(size_t)(i % L)] += 1.0;
        }
    }
    for (int v = 0; v < 4; v++) for (int64_t l = 0; l < L; l++) fold[(size_t)v][(size_t)l] /= std::max(1.0, cnt[(size_t)l]);
    // joint search over d
    double bestTot = -1;
    int64_t bestG = 0;
    int bestD = 0;
    std::vector<double> tot((size_t)L);
    for (int d = -700; d <= 700; d += 4) {
        const int64_t sh[4] = {(int64_t)std::llround(kSwU[0] * d), (int64_t)std::llround(kSwU[1] * d), (int64_t)std::llround(kSwU[2] * d), (int64_t)std::llround(kSwU[3] * d)};
        for (int64_t g = 0; g < L; g++) {
            double s = 0;
            for (int v = 0; v < 4; v++) s += fold[(size_t)v][(size_t)(((g + sh[v]) % L + L) % L)];
            tot[(size_t)g] = s;
            if (s > bestTot) { bestTot = s; bestG = g; bestD = d; }
        }
    }
    // per-period winners and refined peak positions
    std::vector<double> ks, ps;
    int votes[4] = {0, 0, 0, 0};
    std::vector<double> rawPeak, rawU;   // measured peak position and shift factor of every period's winner
    const int64_t sh[4] = {(int64_t)std::llround(kSwU[0] * bestD), (int64_t)std::llround(kSwU[1] * bestD), (int64_t)std::llround(kSwU[2] * bestD), (int64_t)std::llround(kSwU[3] * bestD)};
    for (int64_t k = -1;; k++) {
        double bestE = -1;
        int bv = -1;
        int64_t bl = 0;
        bool any = false;
        for (int v = 0; v < 4; v++) {
            const int64_t c = bestG + L * k + sh[v];
            if (c - 3 < 0 || c + 3 >= nm) continue;
            any = true;
            for (int64_t l = c - 3; l <= c + 3; l++)
                if (mag2[(size_t)v][(size_t)l] > bestE) { bestE = mag2[(size_t)v][(size_t)l]; bv = v; bl = l; }
        }
        if (bestG + L * k - 700 >= nm) break;
        if (!any || bv < 0) continue;
        if (bl <= 0 || bl >= nm - 1) continue;
        const auto& m = mag2[(size_t)bv];
        const double d = parabola(std::sqrt((double)m[(size_t)bl - 1]), std::sqrt((double)m[(size_t)bl]), std::sqrt((double)m[(size_t)bl + 1]));
        ks.push_back((double)k);
        ps.push_back((double)bl + d - kSwU[bv] * (double)bestD);
        rawPeak.push_back((double)bl + d);
        rawU.push_back(kSwU[bv]);
        votes[bv]++;
    }
    if (ks.empty()) return false;
    double sl = 0, ic = 0;
    polyfit1(ks, ps, sl, ic);
    double per = (double)L, t0;
    if (ks.size() >= 3 && std::fabs(sl - (double)L) <= 0.02 * (double)L) { per = sl; t0 = ic; }
    else { double m = 0; for (size_t i = 0; i < ks.size(); i++) m += ps[i] - per * ks[i]; t0 = m / (double)ks.size(); }
    int ref = 0;
    for (int v = 1; v < 4; v++) if (votes[v] > votes[ref]) ref = v;
    seed.t0 = t0; seed.per = per; seed.dshift = (double)bestD; seed.refClass = ref; seed.ok = true;
    seed.dExact = false;
    {   // peak = t0 + k * per + u * d: with two different shift factors among the periods the carrier-offset shift d is fitted exactly (least squares)
        bool two = false;
        for (size_t i = 1; i < rawU.size(); i++) if (std::fabs(rawU[i] - rawU[0]) > 0.25) two = true;
        if (two) {
            double sk = 0, su = 0, sy = 0, skk = 0, suu = 0, sku = 0, sky = 0, suy = 0;   // unknowns t0, d; y = peak - k * per
            const double n = (double)ks.size();
            for (size_t i = 0; i < ks.size(); i++) {
                const double y = rawPeak[i] - per * ks[i], u = rawU[i];
                su += u; sy += y; suu += u * u; suy += u * y;
            }
            (void)sk; (void)skk; (void)sku; (void)sky;
            const double det = n * suu - su * su;
            if (std::fabs(det) > 1e-9) {
                const double dd = (n * suy - su * sy) / det, tt = (sy - dd * su) / n;
                if (std::fabs(dd) < 1500.0) { seed.t0 = tt; seed.dshift = dd; seed.dExact = true; }
            }
        }
    }
    seed.classes = (votes[0] > 0) + (votes[1] > 0) + (votes[2] > 0) + (votes[3] > 0);
    return true;
}

// Streaming sweep remover: tracks the four variant positions, decides the variant of every period by correlation, subtracts it with a
// least-squares complex gain, hands the cleaned samples to the sink in order, and feeds the per-period energies to the ID decoder.
class SweepProc {
public:
    using Sink = std::function<void(const cd*, int64_t, size_t)>;
    Sink sink;
    SweepIdDecoder* ids = nullptr;
    double sp = 0, dp = 0;           // removed power and residual power over the fitted samples
    uint64_t periodsDone = 0, periodsFull = 0;

    void seed(const SweepSeed& s) {
        raw_.clear(); rawBase_ = 0; swLo_ = 0; k_ = 0; sp = dp = 0; periodsDone = periodsFull = 0;
        per_ = s.per;
        d_ = s.dshift;
        // the first period that ends after sample 0 (its start may be negative)
        const double t = s.t0 + s.per * std::floor((0.0 - s.t0) / s.per);
        B_ = t;
        for (int v = 0; v < 4; v++) { pos_[v] = B_ + kSwU[v] * d_; conf_[v] = (v == s.refClass); }
    }
    void feed(const cd* x, size_t n) {
        raw_.insert(raw_.end(), x, x + n);
        for (;;) {
            const int64_t rawEnd = rawBase_ + (int64_t)raw_.size();
            double need = B_ + per_;
            for (int v = 0; v < 4; v++) {
                int a, b;
                sweepVariantSupport(v, a, b);
                need = std::max(need, pos_[v] + b + (conf_[v] ? kSwWin : kSwWinNew));
            }
            if (rawEnd < (int64_t)std::ceil(need) + 6) break;
            period();
        }
    }
    // The carrier offset of the input changed by dHz (a mixer retune): the sweep peaks move by u * d, keep tracking them
    void shiftCarrier(double dHz) {
        const double dd = -dHz * kSwShiftPerHz;
        d_ += dd;
        for (int v = 0; v < 4; v++) pos_[v] += kSwU[v] * dd;
    }
    double ratioDb() const { return (dp > 0 && sp > 0) ? 10 * std::log10(sp / dp) : 0.0; }
    double periodLen() const { return per_; }

private:
    std::vector<cd> raw_;
    int64_t rawBase_ = 0, swLo_ = 0;
    double per_ = kSwL, B_ = 0, d_ = 0, pos_[4] = {0, 0, 0, 0};
    bool conf_[4] = {false, false, false, false};
    uint64_t k_ = 0;

    cd corr(int v, int64_t c) const {   // sum raw[c + i] conj(T[i]) over the support, no bounds check
        const auto& T = sweepTemplatesD()[(size_t)v];
        int a, b;
        sweepVariantSupport(v, a, b);
        cd acc(0, 0);
        const cd* px = &raw_[(size_t)(c - rawBase_)];
        for (int i = a; i < b; i++) acc += px[i] * std::conj(T[(size_t)i]);
        return acc;
    }

    void period() {
        const int64_t rawEnd = rawBase_ + (int64_t)raw_.size();
        double E[4] = {0, 0, 0, 0}, meas[4] = {0, 0, 0, 0};
        bool inner[4] = {false, false, false, false}, fullAll = true;
        for (int v = 0; v < 4; v++) {
            int a, b;
            sweepVariantSupport(v, a, b);
            const int W = conf_[v] ? kSwWin : kSwWinNew;
            const int64_t c0 = (int64_t)std::llround(pos_[v]);
            if (c0 - W < rawBase_ || c0 + W + b > rawEnd) { fullAll = false; continue; }
        }
        if (fullAll) {
            for (int v = 0; v < 4; v++) {
                int a, b;
                sweepVariantSupport(v, a, b);
                const int W = conf_[v] ? kSwWin : kSwWinNew;
                const int64_t c0 = (int64_t)std::llround(pos_[v]);
                std::vector<double> mg((size_t)(2 * W + 1));
                int j = 0;
                for (int dl = 0; dl <= 2 * W; dl++) {
                    mg[(size_t)dl] = std::abs(corr(v, c0 - W + dl));
                    if (mg[(size_t)dl] > mg[(size_t)j]) j = dl;
                }
                E[v] = mg[(size_t)j] * mg[(size_t)j];
                inner[v] = j > 0 && j < 2 * W;
                meas[v] = (double)(c0 - W + j) + (inner[v] ? parabola(mg[(size_t)j - 1], mg[(size_t)j], mg[(size_t)j + 1]) : 0.0);
            }
        } else {
            // partial period (stream start): the variant by the energy captured in the overlap, no position update
            for (int v = 0; v < 4; v++) {
                int a, b;
                sweepVariantSupport(v, a, b);
                const int64_t c = (int64_t)std::llround(pos_[v]);
                const int64_t i0 = std::max<int64_t>(a, rawBase_ - c), i1 = std::min<int64_t>(b, rawEnd - c);
                if (i1 - i0 < 2000) continue;
                const auto& T = sweepTemplatesD()[(size_t)v];
                cd acc(0, 0);
                double te = 0;
                for (int64_t i = i0; i < i1; i++) { acc += raw_[(size_t)(c + i - rawBase_)] * std::conj(T[(size_t)i]); te += std::norm(T[(size_t)i]); }
                E[v] = te > 0 ? std::norm(acc) / te * (double)kSwL : 0.0;   // scaled like a full-period energy
                meas[v] = pos_[v];
            }
        }
        int vs = 0;
        for (int v = 1; v < 4; v++) if (E[v] > E[vs]) vs = v;
        double sUsed = pos_[vs];
        if (fullAll && inner[vs]) {
            const double err = meas[vs] - pos_[vs];
            const double lim = conf_[vs] ? kSwWin - 0.5 : kSwWinNew - 0.5;
            if (std::fabs(err) <= lim) {
                if (!conf_[vs]) { conf_[vs] = true; pos_[vs] = meas[vs]; sUsed = meas[vs]; }
                else { sUsed = pos_[vs] + 0.3 * err; pos_[vs] = sUsed; }
                const double tEst = sUsed - kSwU[vs] * d_;
                B_ += 0.3 * (tEst - B_);
                per_ = std::min(1.02 * kSwL, std::max(0.98 * kSwL, per_ + 0.02 * err));
            }
        }
        if (fullAll && ids) {
            double others = 0;
            for (int v = 0; v < 4; v++) if (v != vs) others += E[v];
            others /= 3.0;
            const double gate = 10 * std::log10((E[vs] + 1e-30) / (others + 1e-30));
            ids->addPeriod(k_, E, gate);
        }
        if (fullAll) periodsFull++;
        // fit and subtract over the tile [swLo, hi)
        const double epsw = per_ / (double)kSwL - 1.0;
        const int64_t lo = std::max<int64_t>(swLo_, 0);
        const int64_t hi = (int64_t)std::ceil(B_ + per_);
        if (hi > lo && lo >= rawBase_) {
            int64_t f0 = lo, f1 = hi;
            if (vs == kSwFast1) f1 = std::min<int64_t>(hi, (int64_t)std::ceil(B_ + per_ / 2));
            else if (vs == kSwFast2) f0 = std::max<int64_t>(lo, (int64_t)std::ceil(B_ + per_ / 2));
            if (f1 - f0 >= 2000 && E[vs] > 0) {
                const size_t n = (size_t)(f1 - f0);
                std::vector<cd> t(n);
                cd num(0, 0);
                double den = 0;
                for (size_t i = 0; i < n; i++) {
                    const double tt = (((double)(f0 + (int64_t)i) - sUsed) / (1.0 + epsw)) / kFs;
                    t[i] = sweepVariantFormula(vs, tt);
                    num += std::conj(t[i]) * raw_[(size_t)(f0 + (int64_t)i - rawBase_)];
                    den += std::norm(t[i]);
                }
                const cd g = den > 0 ? num / den : cd(0, 0);
                for (size_t i = 0; i < n; i++) {
                    cd& xr = raw_[(size_t)(f0 + (int64_t)i - rawBase_)];
                    const cd e = g * t[i];
                    sp += std::norm(e);
                    xr -= e;
                    dp += std::norm(xr);
                }
            }
            if (sink) sink(&raw_[(size_t)(lo - rawBase_)], lo, (size_t)(hi - lo));
            swLo_ = hi;
        }
        periodsDone++;
        k_++;
        B_ += per_;
        for (int v = 0; v < 4; v++) pos_[v] += per_;
        // drop consumed samples, keeping what the next correlations need
        double keep = (double)swLo_;
        for (int v = 0; v < 4; v++) keep = std::min(keep, pos_[v] - (conf_[v] ? kSwWin : kSwWinNew) - 4.0);
        const int64_t keepFrom = (int64_t)std::floor(keep);
        if (keepFrom > rawBase_ + 4096) {
            const size_t cut = (size_t)(keepFrom - rawBase_);
            raw_.erase(raw_.begin(), raw_.begin() + (std::ptrdiff_t)cut);
            rawBase_ += (int64_t)cut;
        }
    }
};

// ---------------------------------------------------------------- timing, sync (batch, acquisition)
void energyPhases(const cd* seg, size_t n, double* E) {
    for (int p = 0; p < kSps; p++) {
        double s = 0;
        size_t cnt = 0;
        for (size_t i = (size_t)p; i + (size_t)kSps < n + (size_t)p; i += kSps) { s += std::norm(seg[i]); cnt++; }
        E[p] = cnt ? s / (double)cnt : 0;
    }
}
void findTimingConst(const std::vector<cd>& mf, double& tau0) {
    const size_t len = std::min(mf.size(), (size_t)(6 * kFs));
    const size_t n = len / kSps * kSps;
    double E[kSps];
    energyPhases(mf.data(), n, E);
    int p = 0;
    for (int i = 1; i < kSps; i++) if (E[i] > E[p]) p = i;
    tau0 = (double)p + parabola(E[(p - 1 + kSps) % kSps], E[p], E[(p + 1) % kSps]);
    tau0 = pymod(tau0, kSps);
}
bool trackTiming(const std::vector<cd>& mf, double& tau0, double& eps) {   // false: no block had any power (silence, NaN)
    const size_t blk = (size_t)kSym * kSps / 4;
    const size_t nb = mf.size() / blk;
    eps = 0;
    if (nb < 4) { findTimingConst(mf, tau0); return true; }
    std::vector<double> pw(nb);
    double pmax = 0;
    for (size_t b = 0; b < nb; b++) {
        double s = 0;
        for (size_t i = b * blk; i < (b + 1) * blk; i++) s += std::norm(mf[i]);
        pw[b] = s / (double)blk;
        pmax = std::max(pmax, pw[b]);
    }
    std::vector<double> taus, cs;
    for (size_t b = 0; b < nb; b++) {
        if (!(pw[b] > 0.5 * pmax)) continue;
        double E[kSps];
        energyPhases(mf.data() + b * blk, blk, E);
        int p = 0;
        for (int i = 1; i < kSps; i++) if (E[i] > E[p]) p = i;
        taus.push_back((double)p + parabola(E[(p - 1 + kSps) % kSps], E[p], E[(p + 1) % kSps]));
        cs.push_back((double)(b * blk) + (double)blk / 2.0);
    }
    bool constant = taus.size() < 3;
    double slope = 0, icpt = 0;
    if (!constant) {
        std::vector<double> ph(taus.size());
        for (size_t i = 0; i < taus.size(); i++) ph[i] = 2 * kPi * taus[i] / kSps;
        ph = unwrapped(ph);
        for (double& v : ph) v = v * kSps / (2 * kPi);
        polyfit1(cs, ph, slope, icpt);
        if (std::fabs(slope) * (cs.back() - cs.front()) < 0.5) constant = true;
    }
    if (taus.empty()) return false;
    if (constant) {
        const double ref = taus[0];
        double dev = 0;
        for (double t : taus) dev += pymod(t - ref + kSps / 2.0, kSps) - kSps / 2.0;
        tau0 = pymod(ref + dev / (double)taus.size(), kSps);
        return true;
    }
    tau0 = pymod(icpt, kSps);
    eps = slope;
    return true;
}

// Differential-correlation preamble metric at symbol j (0 .. n-32).
double syncMetricAt(const cd* y, size_t j, const std::vector<cd>& w) {
    cd num(0, 0);
    double en = 0;
    for (size_t i = 0; i < w.size(); i++) {
        const cd q = y[j + i + 1] * std::conj(y[j + i]);
        num += q * std::conj(w[i]);
        en += std::abs(q);
    }
    return std::abs(num) / (en + 1e-12);
}
const std::vector<cd>& diffPreamble() {
    static const std::vector<cd> w = [] {
        std::vector<cd> v;
        const auto& p = layout().pre;
        for (size_t i = 1; i < p.size(); i++) v.push_back(p[i] * std::conj(p[i - 1]));
        return v;
    }();
    return w;
}

void findFrames(const std::vector<cd>& y, std::vector<int64_t>& starts, double& mmax) {
    starts.clear();
    mmax = 0;
    const int64_t n = (int64_t)y.size();
    if (n < kPreambleLen + 2) return;
    const size_t nm = (size_t)n - 31;
    std::vector<double> m(nm);
    int64_t best = 0;
    for (size_t j = 0; j < nm; j++) { m[j] = syncMetricAt(y.data(), j, diffPreamble()); if (m[j] > m[(size_t)best]) best = (int64_t)j; }
    mmax = m[(size_t)best];
    const double thr = std::max(0.55, 0.5 * mmax);
    if (m[(size_t)best] < 0.3) return;
    std::map<int64_t, double> pos;
    pos[best] = m[(size_t)best];
    for (int dir = 1; dir >= -1; dir -= 2) {
        int64_t cur = best;
        int weak = 0;
        std::vector<int64_t> pend;
        for (;;) {
            const int64_t nxt = cur + dir * kRtFrameSymbols;
            if (nxt < 0 || nxt + kRtFrameSymbols > n) break;
            const int64_t lo = std::max<int64_t>(nxt - 3, 0), hi = std::min<int64_t>(nxt + 4, (int64_t)nm);
            if (lo >= hi) break;
            int64_t j = lo;
            for (int64_t i = lo; i < hi; i++) if (m[(size_t)i] > m[(size_t)j]) j = i;
            if (m[(size_t)j] >= thr) {
                for (int64_t pj : pend) pos[pj] = m[(size_t)pj];
                pend.clear();
                pos[j] = m[(size_t)j]; cur = j; weak = 0;
            } else {
                weak++; cur = nxt; pend.push_back(j);
                if (weak >= kMaxPendingWeak) break;
            }
        }
    }
    for (const auto& kv : pos) if (kv.first + kRtFrameSymbols <= n) starts.push_back(kv.first);
}

struct Static {   // parameters found at acquisition
    cd dc = cd(0, 0);
    double sweepDb = 0, cfoPre = 0, tau0 = 0, eps = 0, rate = 0, cfoHz = 0, mref = 0;
    int64_t firstStart = 0;
};

}  // namespace

// ================================================================= receiver
struct VoiceRx::Impl {
    std::function<void(const std::string&)> log;
    double inRate = kFs;
    RationalResampler rs;
    bool resampling = false;
    RxStats st;
    std::vector<cf32> rsOut;

    // acquisition
    std::vector<cd> acq;           // raw 48 kHz samples (DC not removed)
    size_t acqTry = 0;             // index into kAcqTry of the next acquisition attempt
    size_t minTry = 0;             // after a bad short lock the short tries (index < kLongTry) are skipped
    bool probation = false;        // locked on a short buffer: the first words must decode, else the lock is dropped
    uint64_t probBase = 0, probGood = 0, probKeepGood = 0, probKeepBad = 0;
    bool tracking = false;
    Static P;

    // sweep stage
    SweepProc sw;                  // removes the sweep of the DC-removed input and hands the rest on
    SweepIdDecoder ids;            // the station ID carried in the sweep
    SweepSeed seed;                // what the batch search found
    SweepIdResult idPub;           // the last confirmed ID (kept across re-acquisitions)
    // matched filter
    std::vector<double> taps;
    std::vector<cd> xr;            // rotated, sweep-free samples (history only)
    int64_t xrBase = 0;
    std::vector<cd> mf;
    int64_t mfBase = 0;
    // symbol sampling
    double tNext = 0, step = kSps;
    int64_t symNext = 0;
    std::vector<cd> ysym;
    std::vector<double> ytime;
    int64_t yBase = 0;
    // tracker
    bool locked = false;
    int64_t fstart = 0;
    int weak = 0;
    std::vector<int64_t> pend;
    double nvGlob = 0;
    int64_t nvCount = 0;
    // blocks
    bool aligned = false;
    std::vector<std::vector<float>> alignQ, blockQ;
    KaalCodec3::StreamDecoder dec{0};
    bool primed = false;
    // codec frame assembly from the 480-bit words: a frame of 24 or 32 kbit/s takes two words
    int pairKbit = 0;              // 0 or the 2-word profile of the stream (24 / 32)
    int phase = 0;                 // 0 expecting the start of a codec frame, 1 expecting the second half of a pair
    bool havePending = false;
    std::vector<uint8_t> pending;  // first half of a pair
    int lastK = 240;               // information bits of the last good word, tried first
    std::deque<int16_t> out;

    bool fast = false;             // setFastAcquire: try the short acquisition buffers (off: the 3 s buffer only)
    double wAvg = 0;               // mean residual rotation per symbol (rad) left after the acquisition's rate

    void say(const std::string& s) { if (log) log(s); }

    void restart() {
        acq.clear();
        acqTry = minTry;
        probation = false;
        tracking = false;
        locked = false;
        xr.clear(); xrBase = 0; mf.clear(); mfBase = 0;
        ysym.clear(); ytime.clear(); yBase = 0; symNext = 0;
        pend.clear(); weak = 0;
        aligned = false; alignQ.clear(); blockQ.clear();
        pairKbit = 0; phase = 0; havePending = false; pending.clear();
        ids.reset();
        nvGlob = 0; nvCount = 0;
        st.locked = false;
    }
    void fullReset() {
        minTry = 0;
        restart();
        st = RxStats();
        out.clear();
        primed = false;
        dec.reset();
        idPub = SweepIdResult();
        if (resampling) rs.reset();
    }

    // After the replay of a short buffer: false when it is already clear that the seed is bad (>= 2 bad words among the decoded ones, or the
    // lock was lost). With fewer than 6 words decoded the live probation decides.
    bool shortLockOk() const {
        if (!tracking) return false;
        const uint64_t tot = st.framesGood + st.framesBad - probBase, good = st.framesGood - probGood;
        return !(tot >= 6 && tot - good >= 2);
    }

    // ------------------------------------------------------------ input
    void feed48(const cf32* x, size_t n) {
        if (!n) return;
        if (!tracking) {
            for (size_t i = 0; i < n; i++) acq.emplace_back(x[i].real(), x[i].imag());
            if (acq.size() >= (fast ? std::min(kAcqLen, kAcqTry[acqTry]) : kAcqLen)) {
                if (acquire()) {
                    probation = fast && acqTry < kLongTry;
                    probBase = st.framesGood + st.framesBad; probGood = st.framesGood;
                    probKeepGood = st.framesGood; probKeepBad = st.framesBad;
                    const bool shortTry = probation;
                    acqTry = 0;
                    std::vector<cd> buf;
                    buf.swap(acq);
                    ingest(buf.data(), buf.size());
                    if (shortTry && !shortLockOk()) {
                        // the replay of the buffer itself shows a bad seed: undo, and go on with the longer buffers (nothing was heard)
                        say("KAAL: short-buffer lock rejected on the replay");
                        const uint64_t keepGood = probKeepGood, keepBad = probKeepBad, keepIn = st.samplesIn;
                        out.clear(); dec.reset(); primed = false;
                        minTry = kAcqN - 1;
                        restart();
                        st.framesGood = keepGood; st.framesBad = keepBad; st.samplesIn = keepIn;
                        acq.swap(buf);
                        acqTry = minTry;
                    }
                } else if (acq.size() < kAcqLen) {
                    acqTry++;   // not yet: try again on a longer buffer
                } else {
                    const size_t drop = 2 * (size_t)kFs;   // nothing found: slide the window
                    acq.erase(acq.begin(), acq.begin() + (std::ptrdiff_t)std::min(drop, acq.size()));
                    acqTry = minTry;
                }
            }
        } else {
            std::vector<cd> buf(n);
            for (size_t i = 0; i < n; i++) buf[i] = cd(x[i].real(), x[i].imag());
            ingest(buf.data(), n);
            if (probation) {
                // a seed from one sweep period is coarser than one from three seconds: judge it on the first 16 words, then fall back to
                // the longer buffers (a good lock clears the penalty)
                const uint64_t tot = st.framesGood + st.framesBad - probBase, good = st.framesGood - probGood;
                if (!tracking) { probation = false; minTry = kAcqN - 1; }
                else if (tot >= 16) {
                    probation = false;
                    if (tot - good >= 2) {
                        say("KAAL: short-buffer lock rejected, re-acquiring on a longer buffer");
                        const uint64_t keepGood = st.framesGood, keepBad = st.framesBad, keepIn = st.samplesIn;
                        minTry = kAcqN - 1;
                        out.clear();
                        restart();
                        st.framesGood = keepGood; st.framesBad = keepBad; st.samplesIn = keepIn;
                    } else minTry = 0;
                }
            }
        }
    }

    // ------------------------------------------------------------ acquisition (batch, faithful to decode_stream steps 1-5b)
    bool acquire() {
        Static s;
        std::vector<cd> x(acq);
        cd mean(0, 0);
        for (const cd& v : x) mean += v;
        mean /= (double)x.size();
        for (cd& v : x) v -= mean;
        s.dc = mean;
        SweepSeed sd;
        if (!sweepSearch(x, sd)) return false;
        double ratio = 0;
        {   // first pass: the sweep out of the acquisition buffer with the offset found by the joint search
            std::vector<cd> xc;
            xc.reserve(x.size());
            SweepProc p1;
            p1.sink = [&](const cd* seg, int64_t first, size_t n) { if ((size_t)first == xc.size()) xc.insert(xc.end(), seg, seg + n); };
            p1.seed(sd);
            p1.feed(x.data(), x.size());
            if (xc.size() < (size_t)(kSwL + kSwL / 2)) return false;   // timing, carrier and sync are estimated on 1.5 cleaned periods at least (0.75 s)
            ratio = p1.ratioDb();
            if (fast && acqTry < kLongTry && ratio < -16.0) return false;   // a short buffer must show the sweep clearly (a clean one reads about -11 dB)
            x.swap(xc);
        }
        const size_t N = x.size();
        s.sweepDb = ratio;
        const std::vector<double> tp = rrcTaps();
        const size_t nt = tp.size();
        const Layout& lay = layout();
        std::vector<cd> mfb, y;
        std::vector<int64_t> starts;
        double mmax = 0, om0 = 0, cfo = 0;
        for (int it = 0; it < 2; it++) {
            mfb.assign(N + nt - 1, cd(0, 0));
            for (size_t i = 0; i < N; i++) {
                const cd v = x[i];
                for (size_t k = 0; k < nt; k++) mfb[i + k] += v * tp[k];
            }
            if (!trackTiming(mfb, s.tau0, s.eps)) return false;
            const double stp = kSps * (1.0 + s.eps);
            const int64_t nsym = (int64_t)(((double)mfb.size() - 8.0 - s.tau0) / stp);
            if (nsym < kRtFrameSymbols + 40) return false;
            y.resize((size_t)nsym);
            for (int64_t m = 0; m < nsym; m++) y[(size_t)m] = interpAt(mfb, 0, s.tau0 + stp * (double)m);
            findFrames(y, starts, mmax);
            if (starts.empty()) return false;
            std::vector<double> om;
            for (int64_t s_ : starts) {
                cd acc(0, 0), prev(0, 0);
                for (int i = 0; i < kPreambleLen; i++) {
                    const cd z = y[(size_t)(s_ + i)] * std::conj(lay.pre[(size_t)i]);
                    if (i > 0) acc += z * std::conj(prev);
                    prev = z;
                }
                om.push_back(std::arg(acc));
            }
            std::sort(om.begin(), om.end());
            om0 = om.size() % 2 ? om[om.size() / 2] : 0.5 * (om[om.size() / 2 - 1] + om[om.size() / 2]);
            cfo = om0 * kSym / (2 * kPi);
            if (it == 0 && std::fabs(cfo) > 100) {
                s.cfoPre = cfo;
                for (size_t n = 0; n < N; n++) {
                    const double ph = -2 * kPi * pymod(s.cfoPre * (double)n / kFs, 1.0);
                    x[n] *= cd(std::cos(ph), std::sin(ph));
                }
                continue;
            }
            break;
        }
        // spurious frames: preamble gain much weaker than the median
        std::vector<double> gpre;
        for (int64_t s_ : starts) {
            cd acc(0, 0);
            for (int i = 0; i < kPreambleLen; i++) {
                const double ph = -om0 * i;
                acc += y[(size_t)(s_ + i)] * std::conj(lay.pre[(size_t)i]) * cd(std::cos(ph), std::sin(ph));
            }
            gpre.push_back(std::abs(acc / (double)kPreambleLen));
        }
        std::vector<double> gs(gpre);
        std::sort(gs.begin(), gs.end());
        const double med = gs.size() % 2 ? gs[gs.size() / 2] : 0.5 * (gs[gs.size() / 2 - 1] + gs[gs.size() / 2]);
        std::vector<int64_t> kept;
        for (size_t i = 0; i < starts.size(); i++) {
            bool k = gpre[i] > 0.5 * med;
            if (!k) {
                const bool a = std::find(starts.begin(), starts.end(), starts[i] - kRtFrameSymbols) != starts.end();
                const bool b = std::find(starts.begin(), starts.end(), starts[i] + kRtFrameSymbols) != starts.end();
                k = a && b;
            }
            if (k) kept.push_back(starts[i]);
        }
        if (kept.empty()) return false;
        starts = kept;
        // coarse CFO removal, then the fine slope from preamble means and pilots
        for (size_t m = 0; m < y.size(); m++) { const double ph = -om0 * (double)m; y[m] *= cd(std::cos(ph), std::sin(ph)); }
        std::vector<double> ang, pidx, pang;
        std::vector<char> isp;
        std::vector<double> idx;
        for (int64_t s_ : starts) {
            cd g(0, 0);
            for (int i = 0; i < kPreambleLen; i++) g += y[(size_t)(s_ + i)] * std::conj(lay.pre[(size_t)i]);
            g /= (double)kPreambleLen;
            idx.push_back((double)s_ + (kPreambleLen - 1) / 2.0); ang.push_back(std::arg(g)); isp.push_back(0);
            for (int pi : lay.pilotIdx) {
                idx.push_back((double)(s_ + pi)); ang.push_back(std::arg(y[(size_t)(s_ + pi)] / lay.pilot)); isp.push_back(1);
            }
        }
        ang = unwrapped(ang);
        for (size_t i = 0; i < ang.size(); i++) if (isp[i]) { pidx.push_back(idx[i]); pang.push_back(ang[i]); }
        double slope = 0, ic = 0;
        if (pidx.size() >= 2) polyfit1(pidx, pang, slope, ic);
        const double cfoFine = slope * kSym / (2 * kPi);
        s.rate = om0;
        if (std::fabs(cfoFine) > 10) s.rate += slope;
        s.cfoHz = s.rate * kSym / (2 * kPi) + s.cfoPre;
        s.mref = mmax;
        s.firstStart = starts.front();
        P = s;
        {
            char b[200];
            std::snprintf(b, sizeof b, "KAAL: acquired, %zu frames, sweep %.1f dB, cfo %.1f Hz, clock %.1f ppm", starts.size(), ratio, s.cfoHz, s.eps * 1e6);
            say(b);
        }
        // pipeline init
        taps = tp;
        xr.clear(); xrBase = 0; mf.clear(); mfBase = 0;
        ysym.clear(); ytime.clear(); yBase = 0; symNext = 0;
        tNext = P.tau0; step = kSps * (1.0 + P.eps);
        // the final sweep remover: with one variant in the buffer the offset shift is known better from the data CFO than from the search
        seed = sd;
        if (sd.classes <= 1 || (sd.classes < 4 && fast && acqTry < kLongTry)) seed = reseed(sd, -s.cfoHz * kSwShiftPerHz);
        ids.reset();
        sw = SweepProc();
        sw.ids = &ids;
        sw.sink = [this](const cd* seg, int64_t first, size_t n) { pushCleaned(seg, first, n); };
        sw.seed(seed);
        pairKbit = 0; phase = 0; havePending = false; pending.clear();
        locked = true; fstart = P.firstStart; weak = 0; pend.clear();
        aligned = false; alignQ.clear(); blockQ.clear();
        nvGlob = 0; nvCount = 0; wAvg = 0;
        tracking = true;
        st.locked = true;
        st.sweepDb = (float)ratio;
        st.cfoHz = (float)P.cfoHz;
        return true;
    }

    // ------------------------------------------------------------ streaming pipeline
    void ingest(const cd* x, size_t n) {
        std::vector<cd> dcFree(n);
        for (size_t i = 0; i < n; i++) dcFree[i] = x[i] - P.dc;
        sw.feed(dcFree.data(), n);
        if (ids.poll()) {
            idPub = ids.result();
            say("KAAL: sweep ID " + idPub.callsign);
        }
        trackRun();
        trim();
    }

    void pushCleaned(const cd* seg, int64_t first, size_t n) {
        const double fcfo = P.cfoPre;
        for (size_t i = 0; i < n; i++) {
            cd v = seg[i];
            if (fcfo != 0) {
                const double ph = -2 * kPi * pymod(fcfo * (double)(first + (int64_t)i) / kFs, 1.0);
                v *= cd(std::cos(ph), std::sin(ph));
            }
            xr.push_back(v);
            // matched filter output for this sample
            const int64_t nn = xrBase + (int64_t)xr.size() - 1;
            cd acc(0, 0);
            const size_t nt = taps.size();
            for (size_t k = 0; k < nt; k++) {
                const int64_t idx = nn - (int64_t)k;
                if (idx < xrBase) break;
                acc += xr[(size_t)(idx - xrBase)] * taps[k];
            }
            mf.push_back(acc);
        }
        // keep only the filter history
        const size_t keep = taps.size() + 4;
        if (xr.size() > keep + 4096) {
            const size_t cut = xr.size() - keep;
            xr.erase(xr.begin(), xr.begin() + (std::ptrdiff_t)cut);
            xrBase += (int64_t)cut;
        }
    }

    void produceSymbols() {
        const int64_t mfEnd = mfBase + (int64_t)mf.size();
        const int64_t want = fstart + 3 + kRtFrameSymbols + 2;   // symbols needed for the next frame search
        while (yBase + (int64_t)ysym.size() < want) {
            const int64_t i0 = (int64_t)std::floor(tNext);
            if (i0 + 8 >= mfEnd) break;
            cd y = interpAt(mf, mfBase, tNext);
            const double ph = -P.rate * (double)symNext;
            const double phw = pymod(ph, 2 * kPi);
            y *= cd(std::cos(phw), std::sin(phw));
            ysym.push_back(y);
            ytime.push_back(tNext);
            tNext += step;
            symNext++;
        }
    }

    void trackRun() {
        while (locked) {
            produceSymbols();
            const int64_t yEnd = yBase + (int64_t)ysym.size();
            const int64_t lo = std::max<int64_t>(std::max<int64_t>(fstart - 3, yBase), 0), hi = fstart + 3;
            if (yEnd < hi + kRtFrameSymbols + 1) break;
            int64_t j = lo;
            double mj = -1;
            for (int64_t c = lo; c <= hi; c++) {
                const double m = syncMetricAt(ysym.data() + (c - yBase), 0, diffPreamble());
                if (m > mj) { mj = m; j = c; }
            }
            const double thr = std::max(0.55, 0.5 * P.mref);
            if (mj >= thr) {
                for (int64_t pj : pend) emitFrame(pj, false);
                pend.clear();
                emitFrame(j, true);
                fstart = j + kRtFrameSymbols;
                weak = 0;
            } else {
                weak++;
                pend.push_back(j);
                fstart = fstart + kRtFrameSymbols;
                if (weak >= kMaxPendingWeak) {
                    say("KAAL: sync lost");
                    minTry = kAcqN - 1;   // a lock that was lost is not trusted to a short buffer again: the full 3 s acquisition, as before
                    const uint64_t keepGood = st.framesGood, keepBad = st.framesBad, keepIn = st.samplesIn;
                    const float snr = st.snrDb;
                    restart();
                    st.framesGood = keepGood; st.framesBad = keepBad; st.samplesIn = keepIn; st.snrDb = snr;
                    return;
                }
            }
            // trim symbols no longer needed
            const int64_t keepFrom = fstart - 3 * kRtFrameSymbols - 8;
            if (keepFrom > yBase + 2048) {
                const size_t cut = (size_t)(keepFrom - yBase);
                ysym.erase(ysym.begin(), ysym.begin() + (std::ptrdiff_t)cut);
                ytime.erase(ytime.begin(), ytime.begin() + (std::ptrdiff_t)cut);
                yBase += (int64_t)cut;
            }
        }
    }

    void trim() {
        if (ytime.empty()) return;
        const int64_t keepFrom = (int64_t)std::floor(std::min(ytime.front(), tNext)) - 16;
        if (keepFrom > mfBase + 8192) {
            const size_t cut = (size_t)(keepFrom - mfBase);
            mf.erase(mf.begin(), mf.begin() + (std::ptrdiff_t)cut);
            mfBase += (int64_t)cut;
        }
    }

    // ------------------------------------------------------------ one frame
    void emitFrame(int64_t j, bool strong) {
        const Layout& lay = layout();
        const size_t s = (size_t)(j - yBase);
        if (j < yBase || s + kRtFrameSymbols > ysym.size()) return;   // trimmed away or not there: lost
        const cd* fr = ysym.data() + s;
        // complex-linear gain fit g = a + b t on preamble + pilots
        std::array<int, kPreambleLen + kRtNPilots> kt;
        std::array<cd, kPreambleLen + kRtNPilots> kr;
        for (int i = 0; i < kPreambleLen; i++) { kt[(size_t)i] = i; kr[(size_t)i] = lay.pre[(size_t)i]; }
        for (int g = 0; g < kRtNPilots; g++) { kt[(size_t)(kPreambleLen + g)] = lay.pilotIdx[(size_t)g]; kr[(size_t)(kPreambleLen + g)] = lay.pilot; }
        const double tc = (kRtFrameSymbols - 1) / 2.0;
        double S0 = 0, S1 = 0, S2 = 0;
        cd r0(0, 0), r1(0, 0);
        for (size_t i = 0; i < kt.size(); i++) {
            const double t = kt[i] - tc, w = std::norm(kr[i]);
            const cd c = std::conj(kr[i]) * fr[kt[i]];
            S0 += w; S1 += w * t; S2 += w * t * t;
            r0 += c; r1 += c * t;
        }
        const double det = S0 * S2 - S1 * S1;
        const cd a = (r0 * S2 - r1 * S1) / det, b = (r1 * S0 - r0 * S1) / det;
        double res = 0;
        for (size_t i = 0; i < kt.size(); i++) {
            const double t = kt[i] - tc;
            res += std::norm(fr[kt[i]] - (a + b * t) * kr[i]);
        }
        if (strong) {   // the carrier offset read-out follows the slope of the gain inside the frames (the acquisition estimate came from a short buffer); the rotation itself is not touched
            const double w = std::imag(b / a);
            if (std::fabs(w) < 0.05) {
                wAvg += (wAvg == 0.0 ? 1.0 : 0.05) * (w - wAvg);
                st.cfoHz = (float)(P.cfoHz + wAvg * kSym / (2 * kPi));
            }
        }
        const double nvi = (res / std::max<double>((double)kt.size() - 2, 1.0)) / std::max(std::norm(a), 1e-12);
        nvCount++;
        nvGlob += (nvi - nvGlob) / (double)std::min<int64_t>(nvCount, 400);
        const double nvf = 0.5 * nvi + 0.5 * nvGlob;
        std::vector<cf> syms(lay.dataIdx.size());
        for (size_t i = 0; i < syms.size(); i++) {
            const int idx = lay.dataIdx[i];
            const cd g = a + b * (idx - tc);
            const cd v = fr[idx] / g;
            syms[i] = cf((float)v.real(), (float)v.imag());
        }
        std::vector<float> llr = qam16DemapSoft(syms.data(), syms.size(), (float)std::max(nvf, 1e-3));
        st.snrDb = (float)(-10 * std::log10(std::max(nvGlob, 1e-12)));
        if (strong) timingUpdate(j);
        handleFrame(std::move(llr));
    }

    // Early-late check on the preamble: nudges the symbol clock phase and rate for the symbols not yet sampled.
    void timingUpdate(int64_t j) {
        const Layout& lay = layout();
        cd cp(0, 0), cm(0, 0);
        for (int i = 0; i < kPreambleLen; i++) {
            const int64_t k = j - yBase + i;
            const double t = ytime[(size_t)k];
            const double ph = pymod(-P.rate * (double)(j + i), 2 * kPi);
            const cd rot(std::cos(ph), std::sin(ph));
            const cd ref = std::conj(lay.pre[(size_t)i]);
            cp += interpAt(mf, mfBase, t + 0.5) * rot * ref;
            cm += interpAt(mf, mfBase, t - 0.5) * rot * ref;
        }
        const double ap = std::abs(cp), am = std::abs(cm);
        if (ap + am < 1e-9) return;
        const double d = std::min(1.0, std::max(-1.0, 16.0 * (ap - am) / (ap + am)));
        tNext += 0.02 * d;
        step += 0.0006 * d / kRtFrameSymbols;
        step = std::min(kSps * 1.01, std::max(kSps * 0.99, step));
    }

    // ------------------------------------------------------------ block alignment, LDPC, codec
    void handleFrame(std::vector<float>&& llr) {
        if (!aligned) {
            alignQ.push_back(std::move(llr));
            if ((int)alignQ.size() >= kAlignFrames) align();
            return;
        }
        blockQ.push_back(std::move(llr));
        if (blockQ.size() == (size_t)kRtBlockFrames) decodeBlock();
    }

    std::vector<float> deintBlock(const std::vector<const std::vector<float>*>& rows) {
        std::vector<float> in((size_t)kRtBlockFrames * kRtCodedBits, 0.f);
        for (size_t r = 0; r < (size_t)kRtBlockFrames; r++) if (rows[r]) std::copy(rows[r]->begin(), rows[r]->end(), in.begin() + (std::ptrdiff_t)(r * kRtCodedBits));
        return deinterleave(in);
    }

    // true when the row decodes as a word of k information bits (the shortened code); info receives k bits
    static bool decodeWord(const float* row, int k, uint8_t* info) { return ldpcDecodeRm(row, k, info); }

    void align() {
        // first frame sits at block position `off`; count the rows that decode (as either word size) on the first kAlignRows rows from there
        int best = 0;
        double bestScore = -1;
        for (int off = 0; off < kRtBlockFrames; off++) {
            int okRows = 0, rows = 0;
            const int nBlocks = (off + kAlignRows + 3) / 4;
            for (int b = 0; b < nBlocks; b++) {
                std::vector<const std::vector<float>*> r(kRtBlockFrames, nullptr);
                for (int k = 0; k < kRtBlockFrames; k++) {
                    const int fi = b * 4 + k - off;
                    if (fi >= 0 && fi < (int)alignQ.size()) r[(size_t)k] = &alignQ[(size_t)fi];
                }
                const std::vector<float> d = deintBlock(r);
                for (int k = 0; k < kRtBlockFrames; k++) {
                    const int row = b * 4 + k;
                    if (row < off || row >= off + kAlignRows) continue;
                    rows++;
                    uint8_t info[kLdpcK];
                    if (decodeWord(&d[(size_t)k * kRtCodedBits], lastK, info) || decodeWord(&d[(size_t)k * kRtCodedBits], lastK == 240 ? 320 : 240, info)) okRows++;
                }
            }
            const double sc = rows ? (double)okRows / rows : 0;
            if (sc > bestScore) { bestScore = sc; best = off; }
        }
        const int fb = (kRtBlockFrames - best) % kRtBlockFrames;
        char b[120];
        std::snprintf(b, sizeof b, "KAAL: block offset %d (%.0f%% of the words decode)", best, 100.0 * bestScore);
        say(b);
        aligned = true;
        std::vector<std::vector<float>> q;
        q.swap(alignQ);
        for (size_t i = (size_t)fb; i < q.size(); i++) {
            blockQ.push_back(std::move(q[i]));
            if (blockQ.size() == (size_t)kRtBlockFrames) decodeBlock();
        }
    }

    // One decoded word. info/k: the information bits of a good word, ok false for a word that did not decode.
    void wordDone(bool ok, const uint8_t* info, int k) {
        if (ok) st.framesGood++; else st.framesBad++;
        auto emit = [&](const uint8_t* bits, size_t nbits) {
            int16_t pcm[KaalCodec3::kFrame];
            const int got = dec.push(bits, nbits, pcm);
            if (got) st.bitrateKbps = got;
            if (!primed) { primed = true; return; }   // the first decoded frame is the codec delay
            out.insert(out.end(), pcm, pcm + KaalCodec3::kFrame);
        };
        if (phase == 1) {   // the second half of a 2-word frame
            phase = 0;
            if (ok && havePending && k == (int)(pending.size())) {
                std::vector<uint8_t> f(pending);
                f.insert(f.end(), info, info + k);
                havePending = false;
                emit(f.data(), f.size());
                return;
            }
            havePending = false;
            emit(nullptr, 0);   // lost: conceal one frame
            return;
        }
        if (!ok) {
            if (pairKbit) { phase = 1; havePending = false; }   // a lost first half: the next word is the second half, nothing to join
            else emit(nullptr, 0);
            return;
        }
        lastK = k;
        const int kb = KaalCodec3::bitrateOfHeader(info);   // 12 and 16 are whole frames, 24 and 32 are first halves
        if ((kb == 12 && k == 240) || (kb == 16 && k == 320)) { pairKbit = 0; emit(info, (size_t)k); return; }
        if ((kb == 24 && k == 240) || (kb == 32 && k == 320)) {
            pairKbit = kb;
            pending.assign(info, info + k);
            havePending = true;
            phase = 1;
            return;
        }
        emit(nullptr, 0);   // header and word size disagree
    }

    void decodeBlock() {
        std::vector<const std::vector<float>*> r(kRtBlockFrames);
        for (size_t k = 0; k < (size_t)kRtBlockFrames; k++) r[k] = &blockQ[k];
        const std::vector<float> d = deintBlock(r);
        for (size_t k = 0; k < (size_t)kRtBlockFrames; k++) {
            uint8_t info[kLdpcK];
            const float* row = &d[k * kRtCodedBits];
            // the word size first tried: the second half of a pair has the size of the first, otherwise the size of the last good word
            int kTry = (phase == 1 && havePending) ? (int)pending.size() : lastK;
            if (phase == 1 && !havePending && pairKbit) kTry = (pairKbit == 24) ? 240 : 320;
            bool ok = decodeWord(row, kTry, info);
            int kUsed = kTry;
            if (!ok) { kUsed = kTry == 240 ? 320 : 240; ok = decodeWord(row, kUsed, info); }
            wordDone(ok, info, kUsed);
        }
        blockQ.clear();
    }
};

// ================================================================= public interface
VoiceRx::VoiceRx() : p_(new Impl) {}
VoiceRx::~VoiceRx() = default;

void VoiceRx::configure(double sampleRate) {
    Impl& s = *p_;
    s.inRate = sampleRate > 0 ? sampleRate : (double)kFs;
    s.resampling = std::fabs(s.inRate - (double)kFs) > 1e-6;
    if (s.resampling) s.resampling = s.rs.configure(s.inRate, (double)kFs);
    s.fullReset();
}

void VoiceRx::reset() { p_->fullReset(); }
void VoiceRx::setFastAcquire(bool on) { p_->fast = on; }
void VoiceRx::shiftCarrier(double dHz) {
    Impl& s = *p_;
    if (!s.tracking) return;
    s.P.cfoHz += dHz;
    s.P.rate += 2 * kPi * dHz / kSym;
    s.st.cfoHz = (float)s.P.cfoHz;
    s.sw.shiftCarrier(dHz);
}

void VoiceRx::feed(const std::complex<float>* x, size_t n) {
    Impl& s = *p_;
    s.st.samplesIn += n;
    if (s.resampling) {
        s.rsOut.clear();
        s.rs.process(x, n, s.rsOut);
        s.feed48(s.rsOut.data(), s.rsOut.size());
    } else {
        s.feed48(x, n);
    }
    s.st.locked = s.locked && s.tracking;
}

size_t VoiceRx::popAudio(int16_t* out, size_t maxSamples) {
    Impl& s = *p_;
    const size_t n = std::min(maxSamples, s.out.size());
    for (size_t i = 0; i < n; i++) out[i] = s.out[i];
    s.out.erase(s.out.begin(), s.out.begin() + (std::ptrdiff_t)n);
    return n;
}

RxStats VoiceRx::stats() const {
    RxStats r = p_->st;
    r.idValid = p_->idPub.valid;
    r.idCall = p_->idPub.callsign;
    r.idGateDb = p_->idPub.gateDb;
    return r;
}

void VoiceRx::setLogCallback(std::function<void(const std::string&)> cb) { p_->log = std::move(cb); }

}  // namespace dect2::kaal

// ================================================================= sweep-only station ID scanner
namespace dect2::kaal {

struct SweepIdScanner::Impl {
    double inRate = kFs;
    RationalResampler rs;
    bool resampling = false;
    std::vector<cf32> rsOut;
    std::vector<cf32> buf;             // the last kKeep samples at 48 kHz
    uint64_t sinceScan = 0;
    static constexpr size_t kKeep = (size_t)36 * kFs;      // 36 s = 71 periods = 3 ID cycles
    static constexpr size_t kMin = (size_t)26 * kFs;       // first scan: 2 cycles and a bit
    static constexpr size_t kEvery = (size_t)10 * kFs;
    bool synchronous = false;
    // worker
    std::mutex mu;
    std::condition_variable cv, cvDone;
    std::thread th;
    bool stop = false, job = false, busy = false;
    std::vector<cf32> jobBuf;
    // published under mu
    SweepIdResult last;
    float lastGate = 0;
    bool fresh = false;
    uint64_t scans = 0;
    uint64_t generation = 0;           // bumped by reset(): a scan of older data is dropped

    ~Impl() {
        {
            std::lock_guard<std::mutex> lk(mu);
            stop = true;
        }
        cv.notify_all();
        if (th.joinable()) th.join();
    }

    void feed48(const cf32* x, size_t n) {
        buf.insert(buf.end(), x, x + n);
        if (buf.size() > kKeep) buf.erase(buf.begin(), buf.begin() + (std::ptrdiff_t)(buf.size() - kKeep));
        sinceScan += n;
        if (buf.size() < kMin || sinceScan < kEvery) return;
        if (synchronous) {
            sinceScan = 0;
            publish(scanBuf(buf), generation);
            return;
        }
        std::lock_guard<std::mutex> lk(mu);
        if (job || busy) return;   // still working on the last one: try again with the next chunk
        sinceScan = 0;
        jobBuf = buf;
        job = true;
        if (!th.joinable()) th = std::thread([this] { worker(); });
        cv.notify_all();
    }

    struct ScanOut { bool ok = false; SweepIdResult r; float gate = 0; };

    static ScanOut scanBuf(const std::vector<cf32>& b) {
        ScanOut o;
        std::vector<cd> x(b.size());
        cd mean(0, 0);
        for (size_t i = 0; i < b.size(); i++) { x[i] = cd(b[i].real(), b[i].imag()); mean += x[i]; }
        mean /= (double)x.size();
        for (cd& v : x) v -= mean;
        SweepSeed sd;
        if (!sweepSearch(x, sd)) return o;
        SweepIdDecoder dec;
        SweepProc sp;
        sp.ids = &dec;
        sp.seed(sd);
        sp.feed(x.data(), x.size());
        o.ok = true;
        o.gate = (float)dec.gateMean();
        if (dec.result().valid) o.r = dec.result();
        return o;
    }

    void publish(const ScanOut& o, uint64_t gen) {
        std::lock_guard<std::mutex> lk(mu);
        if (gen != generation) return;
        scans++;
        if (!o.ok) return;
        lastGate = o.gate;
        if (o.r.valid) {
            if (!last.valid || last.callsign != o.r.callsign) { last = o.r; fresh = true; }
            else last.gateDb = o.r.gateDb;
        }
    }

    void worker() {
        std::unique_lock<std::mutex> lk(mu);
        for (;;) {
            cv.wait(lk, [this] { return stop || job; });
            if (stop) return;
            std::vector<cf32> b;
            b.swap(jobBuf);
            job = false;
            busy = true;
            const uint64_t gen = generation;
            lk.unlock();
            const ScanOut o = scanBuf(b);
            lk.lock();
            if (gen == generation) {
                scans++;
                if (o.ok) {
                    lastGate = o.gate;
                    if (o.r.valid) {
                        if (!last.valid || last.callsign != o.r.callsign) { last = o.r; fresh = true; }
                        else last.gateDb = o.r.gateDb;
                    }
                }
            }
            busy = false;
            cvDone.notify_all();
        }
    }
};

SweepIdScanner::SweepIdScanner() : p_(new Impl) {}
SweepIdScanner::~SweepIdScanner() = default;
void SweepIdScanner::setSynchronous(bool on) { p_->synchronous = on; }
void SweepIdScanner::configure(double sampleRate) {
    Impl& s = *p_;
    s.inRate = sampleRate > 0 ? sampleRate : (double)kFs;
    s.resampling = std::fabs(s.inRate - (double)kFs) > 1e-6;
    if (s.resampling) s.resampling = s.rs.configure(s.inRate, (double)kFs);
    reset();
}
void SweepIdScanner::reset() {
    Impl& s = *p_;
    s.buf.clear();
    s.sinceScan = 0;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.generation++;
        s.job = false;
        s.jobBuf.clear();
        s.last = SweepIdResult();
        s.fresh = false;
        s.lastGate = 0;
    }
    if (s.resampling) s.rs.reset();
}
void SweepIdScanner::feed(const std::complex<float>* x, size_t n) {
    Impl& s = *p_;
    if (!n) return;
    if (s.resampling) {
        s.rsOut.clear();
        s.rs.process(x, n, s.rsOut);
        s.feed48(s.rsOut.data(), s.rsOut.size());
    } else {
        s.feed48(x, n);
    }
}
void SweepIdScanner::wait() {
    std::unique_lock<std::mutex> lk(p_->mu);
    p_->cvDone.wait(lk, [this] { return !p_->job && !p_->busy; });
}
bool SweepIdScanner::poll(SweepIdResult& r) {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.fresh) return false;
    s.fresh = false;
    r = s.last;
    return true;
}
SweepIdResult SweepIdScanner::last() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->last; }
uint64_t SweepIdScanner::scans() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->scans; }
float SweepIdScanner::lastGateDb() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->lastGate; }

}  // namespace dect2::kaal
