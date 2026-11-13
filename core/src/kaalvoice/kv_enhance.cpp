// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL voice codec 4: decoder-side speech enhancer (see kv_enhance.h and docs/KAAL-ENHANCE.md).
#include "kv_enhance.h"
#include <cmath>
#include <cstring>

namespace dect2 {
namespace kaalvoice {
namespace detail {

namespace {
// minBits20, gn, gd, ltp, tilt, fill, bwe, restoreDb. Tuned with the objective proxies (docs/KAAL-ENHANCE.md section 4) on the
// repo's speech sample and synthetic voices seeds 11..14, 21, 77; the tests check held-out seeds.
const EnhTier kEnhTiers[] = {
    {0,   0.55, 0.72, 0.50, 0.45, 0.30, 0.30, 2.0},   // 3.2
    {80,  0.55, 0.71, 0.45, 0.45, 0.00, 0.30, 1.0},   // 4.8
    {110, 0.55, 0.70, 0.40, 0.45, 0.00, 0.30, 0.8},   // 6.0
    {140, 0.60, 0.75, 0.40, 0.80, 0.00, 0.30, 0.5},   // 8.0
    {200, 0.65, 0.75, 0.25, 0.80, 0.00, 0.30, 0.4},   // HQ12
    {280, 0.70, 0.75, 0.15, 0.80, 0.00, 0.30, 0.3},   // HQ16
};
constexpr int kNEnhTiers = sizeof(kEnhTiers) / sizeof(kEnhTiers[0]);

int bits20(const Prof& P) { return P.totalBits * 20 / P.frameMs; }
double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
double dotf(const float* a, const float* b, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) s += (double)a[i] * b[i];
    return s;
}
}  // namespace

const EnhTier& enhTierOf(const Prof& P) {
    const int b = bits20(P);
    int t = 0;
    for (int i = 0; i < kNEnhTiers; i++)
        if (b >= kEnhTiers[i].minBits20) t = i;
    return kEnhTiers[t];
}

bool enhanceActive(const Config& c, const Prof& P) {
    if (c.enhance == 0) return false;
    if (c.enhance > 0) return true;
    return bits20(P) < 140;
}

int enhanceDelay(const Config& c, const Prof& P) {
    if (!enhanceActive(c, P) || !(c.enhanceStages & kEnhNoiseReduce)) return 0;
    return c.sampleRate == 16000 ? 80 : 40;
}

// ---------------------------------------------------------------- optional noise reduction (output rate)
// 50 % overlapping sqrt-Hann frames of two hops, decision-directed Wiener gain with a -8 dB floor. The noise estimate is the decoder's
// own: learnt from the comfort noise when the encoder sends SIDs, otherwise a minimum tracker on the decoded speech. Delay one hop.
struct Enhancer::Nr {
    int M = 80, W = 160, F = 256;
    float win[160];
    float prev[80], tail[80];
    float Nn[129], G[129], gam[129];
    int frames = 0;
    explicit Nr(int fs) {
        M = fs == 16000 ? 80 : 40;
        W = 2 * M;
        F = fs == 16000 ? 256 : 128;
        for (int n = 0; n < W; n++) win[n] = (float)kvSin(kPi * (n + 0.5) / W);
        reset();
    }
    void reset() {
        std::memset(prev, 0, sizeof prev);
        std::memset(tail, 0, sizeof tail);
        for (int k = 0; k <= 128; k++) { Nn[k] = 0; G[k] = 1; gam[k] = 1; }
        frames = 0;
    }
    void hop(const float* in, float* out, bool learn) {
        float re[256], im[256];
        for (int n = 0; n < F; n++) { re[n] = 0; im[n] = 0; }
        for (int n = 0; n < M; n++) { re[n] = prev[n] * win[n]; re[M + n] = in[n] * win[M + n]; }
        std::memcpy(prev, in, sizeof(float) * (size_t)M);
        fft(re, im, F, false);
        for (int k = 0; k <= F / 2; k++) {
            const float p = re[k] * re[k] + im[k] * im[k];
            if (frames == 0) Nn[k] = p;
            else if (learn) Nn[k] = 0.8f * Nn[k] + 0.2f * p;
            else Nn[k] = p < Nn[k] * 1.003f ? (p > Nn[k] ? p : 0.9f * Nn[k] + 0.1f * p) : Nn[k] * 1.003f;
            const float nk = Nn[k] + 1e-3f;
            const float g = p / nk;
            const float xi = 0.96f * G[k] * G[k] * gam[k] + 0.04f * (g > 1.0f ? g - 1.0f : 0.0f);
            float gk = xi / (1.0f + xi);
            if (gk < 0.4f) gk = 0.4f;
            G[k] = gk;
            gam[k] = g;
            re[k] *= gk;
            im[k] *= gk;
            if (k > 0 && k < F / 2) { re[F - k] = re[k]; im[F - k] = -im[k]; }
        }
        fft(re, im, F, true);
        const float sc = 1.0f / F;
        for (int n = 0; n < M; n++) out[n] = tail[n] + re[n] * sc * win[n];
        for (int n = 0; n < M; n++) tail[n] = re[M + n] * sc * win[M + n];
        frames++;
    }
};

// ---------------------------------------------------------------- enhancer
Enhancer::Enhancer(const Prof& P, uint32_t stages, bool postfilter) : P_(P), T_(enhTierOf(P)), st_(stages), pf_(postfilter) {
    if (st_ & kEnhNoiseReduce) nr_.reset(new Nr(P.fs));
    reset();
}
Enhancer::~Enhancer() = default;

void Enhancer::reset() {
    rng_.s = 0x454e4831u;
    hbRng_.s = 0x454e4832u;
    std::memset(nfSyn_, 0, sizeof nfSyn_);
    std::memset(pfRes_, 0, sizeof pfRes_);
    std::memset(pfFir_, 0, sizeof pfFir_);
    std::memset(pfIir_, 0, sizeof pfIir_);
    pfTilt_ = 0;
    pfAgc_ = 1.0;
    lostRun_ = 0;
    cngE_ = 0;
    cngG_ = 1.0;
    exUp_ = Up2();
    std::memset(hbOla_, 0, sizeof hbOla_);
    for (double& d : hbLs_) d = -100.0;
    hbHave_ = false;
    last_[0] = last_[1] = 0;
    env_ = 0;
    noiseDb_ = -100;
    if (nr_) nr_->reset();
}

void Enhancer::restart() {
    std::memset(pfRes_, 0, sizeof pfRes_);
    std::memset(nfSyn_, 0, sizeof nfSyn_);
    lostRun_ = 0;
}

void Enhancer::postfilter(const SubInfo& si, const float* in, float* out, int L, double v, bool lost) {
    const bool doF = pf_ && (st_ & kEnhFormant), doP = pf_ && (st_ & kEnhPitch);
    constexpr int RH = kPitMax + 2;
    const double gn = T_.gn;
    // formant emphasis: full on voiced subframes, weaker on unvoiced (no whistling noise) and concealed ones
    double w = doF ? 0.55 + 0.45 * clamp01((v - 0.2) / 0.5) : 0.0;
    if (lost) w *= 0.6;
    const double gd = gn + w * (T_.gd - gn);
    float an[kP + 1], ad[kP + 1];
    weightLpc(si.aq, gn, an);
    weightLpc(si.aq, gd, ad);
    float* res = pfRes_ + RH;
    firA(an, in, res, L, pfFir_);
    // long-term post-filter on the residual, around the decoded lag, only for clearly periodic subframes
    double gl = 0.0;
    int bt = si.Ti;
    if (doP && si.gp > 0.3 && !(lost && lostRun_ > 2)) {
        double bc = -1, be = 1;
        const double rr = dotf(res, res, L) + 1e-3;
        for (int T = si.Ti - 1; T <= si.Ti + 1; T++) {
            if (T < kPitMin || T > kPitMax) continue;
            double c = 0, e = 0;
            for (int n = 0; n < L; n++) { c += (double)res[n] * res[n - T]; e += (double)res[n - T] * res[n - T]; }
            if (c > 0 && c * c * be > bc * bc * (e + 1e-3)) { bc = c; be = e + 1e-3; bt = T; }
        }
        if (bc > 0) {
            const double rho = bc / std::sqrt(be * rr);
            if (rho > 0.5) gl = T_.ltp * (bc / be > 1.0 ? 1.0 : bc / be) * clamp01((rho - 0.5) / 0.3);
            if (lost) gl *= 0.5;
        }
    }
    float r2[80], y[80];
    for (int n = 0; n < L; n++) r2[n] = (float)((res[n] + gl * res[n - bt]) / (1.0 + gl));
    iirA(ad, r2, y, L, pfIir_);
    // tilt compensation from the first two autocorrelation lags of the post-filter's impulse response
    if (doF) {
        float hi[22] = {}, ho[22], m2[kP] = {};
        for (int i = 0; i <= kP; i++) hi[i] = an[i];
        iirA(ad, hi, ho, 22, m2);
        double h0 = 0, h1 = 0;
        for (int i = 0; i < 22; i++) h0 += (double)ho[i] * ho[i];
        for (int i = 0; i < 21; i++) h1 += (double)ho[i] * ho[i + 1];
        const double mu = h1 > 0 ? T_.tilt * h1 / (h0 + 1e-9) : 0.0;
        for (int n = 0; n < L; n++) {
            const float t = y[n];
            y[n] = t - (float)mu * pfTilt_;
            pfTilt_ = t;
        }
    } else if (L > 0) pfTilt_ = y[L - 1];
    // gain control to the energy before the post-filter
    const double ein = dotf(in, in, L), eout = dotf(y, y, L);
    const double g = eout > 1e-3 ? std::sqrt(ein / eout) : 0.0;
    for (int n = 0; n < L; n++) {
        pfAgc_ = 0.9 * pfAgc_ + 0.1 * g;
        out[n] = (float)(y[n] * pfAgc_);
    }
    std::memmove(pfRes_, pfRes_ + L, sizeof(float) * RH);
}

void Enhancer::lowBand(const CoreDecoder& core, bool lost, float* lb) {
    const int L = P_.L;
    lostRun_ = lost ? lostRun_ + 1 : 0;
    double e = 0;
    for (int s = 0; s < P_.S; s++) {
        const SubInfo& si = core.sub(s);
        float x[80], nf[80];
        std::memcpy(x, lb + s * L, sizeof(float) * (size_t)L);
        // energy restoration: a sparse pulse codebook leaves the noisy part of the excitation out (the joint gain search scales it
        // down); add noise of the missing power, shaped by the synthesis filter, in proportion to how unvoiced the subframe is
        float ex[80];
        double pn = 0;
        if ((st_ & kEnhLoudness) && !si.rnd && si.gc > 0 && si.ecb > 0) {
            const double u = 1.0 - clamp01(si.gp);
            pn = T_.fill * u * u * si.gc * si.gc * si.ecb;
        }
        const float sd = (float)std::sqrt(pn);
        for (int n = 0; n < L; n++) ex[n] = sd * rng_.gauss();
        iirA(si.aq, ex, nf, L, nfSyn_);
        for (int n = 0; n < L; n++) x[n] += nf[n];
        if (pf_ && (st_ & (kEnhPitch | kEnhFormant))) postfilter(si, x, lb + s * L, L, si.gp, lost);
        else std::memcpy(lb + s * L, x, sizeof(float) * (size_t)L);
        if ((st_ & kEnhLoudness) && T_.restoreDb != 0.0) {
            const float g = (float)kvPow10(T_.restoreDb / 20.0);
            for (int n = 0; n < L; n++) lb[s * L + n] *= g;
        }
        e += dotf(lb + s * L, lb + s * L, L);
    }
    const double db = kvDb(e / P_.N8 + 1e-3);
    noiseDb_ = noiseDb_ < -90 ? db : (db < noiseDb_ ? 0.6 * noiseDb_ + 0.4 * db : noiseDb_ + 0.05 * P_.frameMs / 20.0);
}

void Enhancer::comfortLow(float* lb) {
    const int n = P_.N8;
    const double e = dotf(lb, lb, n) / n + 1e-3;
    noiseDb_ = kvDb(e);
    if (!(st_ & kEnhComfort)) return;
    // the level of a few hundred noise samples fluctuates by chance: smooth it (about 100 ms)
    cngE_ = cngE_ <= 0 ? e : 0.7 * cngE_ + 0.3 * e;
    double g = std::sqrt(cngE_ / e);
    g = g < 0.7 ? 0.7 : (g > 1.4 ? 1.4 : g);
    for (int i = 0; i < n; i++) lb[i] *= (float)(cngG_ + (g - cngG_) * (i + 1) / n);
    cngG_ = g;
}

void Enhancer::highBand(const CoreDecoder* core, const double (*Lv)[kHbBands], int nb, float* hb) {
    const bool smooth = (st_ & kEnhHbSmooth) != 0, bwe = (st_ & kEnhBwe) && core;
    const int N8 = P_.N8, n16 = 2 * N8;
    // excitation at 16 kHz (aligned with the output low band), rectified: its harmonics of F0 reach into the high band and keep the
    // timing of the pitch pulses. 80 samples past the frame (the last block's MDCT window) by pitch repetition.
    float r16[640 + kHbM];
    if (bwe) {
        float u[kMaxN8 + 40];
        std::memcpy(u, core->excitation(), sizeof(float) * (size_t)N8);
        int T = core->sub(P_.S - 1).Ti;
        if (T < kPitMin) T = kPitMin;
        for (int k = 0; k < 40; k++) u[N8 + k] = u[N8 + k - T];
        exUp_.run(u, N8, r16);
        Up2 tmp = exUp_;
        tmp.run(u + N8, 40, r16 + n16);
        for (int i = 0; i < n16 + kHbM; i++) r16[i] = r16[i] < 0 ? -r16[i] : r16[i];
    } else if (st_ & kEnhBwe) {
        float z[kMaxN8] = {};
        exUp_.run(z, N8, r16);   // keep the interpolator's history in step (comfort noise has no excitation)
    }
    for (int b = 0; b < nb; b++) {
        double L[kHbBands];
        for (int j = 0; j < kHbBands; j++) {
            L[j] = Lv[b][j];
            if (smooth) {
                // fast attack, slower release, in dB per 5 ms block: no 6 dB level steps between frames (chirping)
                if (!hbHave_) hbLs_[j] = L[j];
                else hbLs_[j] += (L[j] > hbLs_[j] ? 0.9 : 0.7) * (L[j] - hbLs_[j]);
                L[j] = hbLs_[j];
            }
        }
        hbHave_ = true;
        double v = 0;
        float Xh[kHbM];
        if (bwe) {
            int s = (b * 40 + 16) / P_.L;
            if (s >= P_.S) s = P_.S - 1;
            v = T_.bwe * clamp01((core->sub(s).gp - 0.35) / 0.5);
            if (v > 0) mdct80(r16 + b * kHbM, Xh);
        }
        float X[kHbM] = {}, y[2 * kHbM];
        for (int j = 0; j < kHbBands; j++) {
            if (L[j] < -60.0) continue;
            const double a = std::sqrt(kvFromDb(L[j]));
            const int k0 = kHbEdge[j], k1 = kHbEdge[j + 1];
            // harmonic share falls with frequency
            double vj = v * (1.0 - 0.1 * j);
            double ah = 0;
            if (vj > 0) {
                double eh = 0;
                for (int k = k0; k < k1; k++) eh += (double)Xh[k] * Xh[k];
                eh /= (k1 - k0);
                if (eh > 1e-6) ah = std::sqrt(vj / eh);
                else vj = 0;
            }
            const double an = std::sqrt(1.0 - vj);
            for (int k = k0; k < k1; k++) {
                const double nz = smooth ? ((hbRng_.next() & 0x10000) ? 1.0 : -1.0) : 1.7320508 * hbRng_.uni();
                X[k] = (float)(a * (an * nz + (ah > 0 ? ah * Xh[k] : 0.0)));
            }
        }
        imdct80(X, y);
        for (int n = 0; n < kHbM; n++) hb[b * kHbM + n] = hbOla_[n] + y[n];
        std::memcpy(hbOla_, y + kHbM, sizeof hbOla_);
    }
}

void Enhancer::output(float* x, int n, Kind k, bool afterLoss, int lostRun) {
    const bool wide = P_.fs == 16000;
    if ((st_ & kEnhDeclick) && afterLoss && k == Speech) {
        // 1. a step between the concealed signal and the first good frame: remove it with an offset that decays in about 1 ms
        // (only a step far above the usual sample-to-sample change of the last millisecond counts)
        const double d = (double)last_[0] - x[0];
        if (std::fabs(d) > 4.0 * last_[1] + 64.0) {
            const double r = wide ? 0.94 : 0.88;
            double f = d;
            for (int i = 0; i < n && (f > 0.5 || f < -0.5); i++) { x[i] += (float)f; f *= r; }
        }
        // 2. a burst: the first good frame after a short loss starts from a different excitation history; limit its level to 12 dB
        // above the recent output for the first 10 ms (gains ramp per 2.5 ms block)
        const int lostMs = lostRun * P_.frameMs;
        if (lostMs <= 120 && env_ > 100.0) {
            const int B = wide ? 40 : 20, nbk = (wide ? 160 : 80) / B;
            double g0 = 1.0;
            for (int b = 0; b <= nbk && (b + 1) * B <= n; b++) {
                double g = 1.0;
                if (b < nbk) {
                    const double rms = std::sqrt(dotf(x + b * B, x + b * B, B) / B);
                    const double lim = 4.0 * env_;
                    if (rms > lim) g = lim / rms;
                    if (g < 0.25) g = 0.25;
                }
                for (int i = 0; i < B; i++) x[b * B + i] *= (float)(g0 + (g - g0) * (i + 1) / B);
                g0 = g;
                if (b == nbk) break;
            }
        }
    }
    if (nr_) {
        float y[640];
        const int M = nr_->M;
        for (int i = 0; i < n; i += M) nr_->hop(x + i, y + i, k == Comfort);
        std::memcpy(x, y, sizeof(float) * (size_t)n);
    }
    // recent output level (10 ms) and the last two samples
    const int m = (wide ? 160 : 80) < n ? (wide ? 160 : 80) : n;
    env_ = std::sqrt(dotf(x + n - m, x + n - m, m) / m);
    const int q = wide ? 16 : 8;
    double sd = 0;
    for (int i = n - q; i < n; i++) sd += std::fabs((double)x[i] - x[i - 1]);
    last_[1] = (float)(sd / q);   // mean absolute sample-to-sample change of the last 1 ms
    last_[0] = x[n - 1];
}

}  // namespace detail
}  // namespace kaalvoice
}  // namespace dect2
