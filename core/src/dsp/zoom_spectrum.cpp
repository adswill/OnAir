// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The zoom spectrum, its plan maths and the display window functions (zoom_spectrum.h, docs/WATERFALL-4.0.md).
#include "dect2/dsp/zoom_spectrum.h"
#include "dect2/dsp/dsp_compat.h"
#include "dect2/dsp/spectrum.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <mutex>

namespace dect2 {

static constexpr double kPi = 3.14159265358979323846;

// ---- windows ----------------------------------------------------------------------------------------------------------------------------
namespace specwin {

const char* name(int t) {
    switch (t) {
    case BlackmanHarris: return "Blackman-Harris";
    case Nuttall: return "Nuttall";
    case FlatTop: return "Flat top";
    case Rect: return "Rectangular";
    default: return "Hann";
    }
}

void make(int t, float* w, int n) {
    if (n <= 0) return;
    if (t <= Hann || t >= NumTypes) { hannWindowNorm(w, n); return; }
    // the cosine sums, periodic (i / n), as the analyser's Hann
    static const double kBh[4] = {0.35875, 0.48829, 0.14128, 0.01168};
    static const double kNut[4] = {0.3635819, 0.4891775, 0.1365995, 0.0106411};
    static const double kFt[5] = {0.21557895, 0.41663158, 0.277263158, 0.083578947, 0.006947368};
    const double* a = t == BlackmanHarris ? kBh : t == Nuttall ? kNut : kFt;
    const int terms = t == FlatTop ? 5 : 4;
    double ss = 0;
    std::vector<double> v((size_t)n);
    for (int i = 0; i < n; i++) {
        double s = 0;
        if (t == Rect) s = 1;
        else for (int k = 0; k < terms; k++) s += (k & 1 ? -a[k] : a[k]) * std::cos(2.0 * kPi * k * i / n);
        v[(size_t)i] = s; ss += s * s;
    }
    const double g = ss > 0 ? std::sqrt((double)n / ss) : 1.0;   // unit rms
    for (int i = 0; i < n; i++) w[i] = (float)(v[(size_t)i] * g);
}

double enbwBins(int t) {
    switch (t) {
    case BlackmanHarris: return 2.0044;
    case Nuttall: return 1.9761;
    case FlatTop: return 3.7702;
    case Rect: return 1.0;
    default: return 1.5;
    }
}

}  // namespace specwin

// ---- plan -------------------------------------------------------------------------------------------------------------------------------
namespace zoomfft {

void split(int decim, int& cic, int& halfbands) {
    int l = 0;
    while ((1 << (l + 1)) <= decim) l++;
    if (l <= 2) { halfbands = l; cic = 1; return; }
    halfbands = std::max(2, l - 10);
    cic = 1 << (l - halfbands);
}

Plan make(double fs, double offsetHz, double spanHz, int px, size_t maxBins) {
    Plan p;
    if (!(fs > 0) || !(spanHz > 0) || px < 1 || !std::isfinite(offsetHz) || !std::isfinite(spanHz)) return p;
    int D = 1;
    while (D * 2 <= kMaxDecim && spanHz <= kFill * fs / (double)(D * 2)) D *= 2;
    if (D < 2) return p;
    const size_t want = (size_t)std::ceil(2.0 * (double)px / kFill);
    size_t cap = kMinBins;
    while (cap * 2 <= std::min(maxBins, kMaxBins)) cap *= 2;
    size_t N = kMinBins;
    while (N < want && N < cap) N *= 2;
    p.on = true;
    p.decim = D;
    split(D, p.cic, p.halfbands);
    p.bins = N;
    p.rateHz = fs / (double)D;
    p.binHz = p.rateHz / (double)N;
    p.offsetHz = std::round(offsetHz / p.binHz) * p.binHz;
    return p;
}

bool covers(const Plan& cur, double fs, double offsetHz, double spanHz, int px, size_t maxBins) {
    if (!cur.on || !(fs > 0)) return false;
    if (std::fabs(cur.rateHz - fs / (double)cur.decim) > 1e-9 * fs) return false;   // another sample rate
    const Plan m = make(fs, offsetHz, spanHz, px, maxBins);
    if (!m.on || !m.sameShape(cur)) return false;
    const double eps = 1e-9 * fs;
    return offsetHz - spanHz * 0.5 >= cur.usableLo() - eps && offsetHz + spanHz * 0.5 <= cur.usableHi() + eps;
}

Plan update(const Plan& cur, double fs, size_t fullBins, double offsetHz, double spanHz, int px, size_t maxBins) {
    Plan off; off.id = cur.id;
    if (!(fs > 0) || fullBins == 0 || px < 1 || !(spanHz > 0)) return off;
    if (fs / (double)fullBins <= spanHz / (double)px) return off;   // the full-band FFT already has a bin per pixel
    if (covers(cur, fs, offsetHz, spanHz, px, maxBins)) return cur;
    Plan p = make(fs, offsetHz, spanHz, px, maxBins);
    p.id = cur.id + 1;
    if (!p.on) { off.id = p.id; return off; }
    return p;
}

}  // namespace zoomfft

// ---- decimator --------------------------------------------------------------------------------------------------------------------------
namespace {
constexpr int kHbL = 23;   // the half-band reaches 23 samples either side: 47 taps, 12 of them odd (the even ones are 0 but the centre)
struct HbTaps {
    float h[(kHbL + 1) / 2];
    HbTaps() {
        auto i0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 40; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
        const double beta = 7.5, M = kHbL + 1;
        double sum = 0;
        for (int j = 0; j < (kHbL + 1) / 2; j++) {
            const int k = 2 * j + 1;
            const double r = (double)k / M;
            const double win = i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0(beta);
            const double v = std::sin(kPi * k / 2.0) / (kPi * k) * win;
            h[j] = (float)v; sum += 2 * v;
        }
        const double g = 0.5 / std::max(1e-9, sum);   // DC gain exactly 1: 0.5 + 2 sum h = 1
        for (float& v : h) v = (float)(v * g);
    }
};
const HbTaps& hbTaps() { static const HbTaps t; return t; }
}  // namespace

int ZoomDecimator::halfbandTaps() { return 2 * kHbL + 1; }
const float* ZoomDecimator::halfbandCoefs() { return hbTaps().h; }

void ZoomDecimator::setup(double fs, double offsetHz, int cic, int halfbands) {
    fs_ = fs;
    w_ = fs > 0 ? -2.0 * kPi * offsetHz / fs : 0.0;
    R_ = std::max(1, std::min(zoomfft::kMaxCic, cic));
    H_ = std::max(0, std::min(16, halfbands));
    int l = 0; while ((1 << l) < R_) l++;
    // input clamped to +-8 (2^3) in units of 2^scaleBits; 4 integrators grow it by 4 l bits: 3 + scaleBits + 4 l <= 62
    const int scaleBits = std::min(40, 59 - 4 * l);
    scale_ = std::ldexp(1.0, scaleBits);
    outScale_ = std::ldexp(1.0, -(scaleBits + 4 * l));
    shift_ = l;
    reset();
}

void ZoomDecimator::reset() {
    ph_ = 0; cnt_ = 0;
    std::memset(ig_, 0, sizeof ig_); std::memset(dl_, 0, sizeof dl_);
    hb_.assign((size_t)H_, Hb());
    for (Hb& h : hb_) { h.buf.assign((size_t)kHbL, cf32(0.f, 0.f)); h.next = kHbL; }
}

void ZoomDecimator::run(const cf32* x, size_t n, std::vector<cf32>& out) {
    if (!n) return;
    std::vector<cf32>& a = H_ > 0 ? a_ : out;   // with no half-band the CIC writes straight out
    if (H_ > 0) a.clear();
    // mixer + CIC, in pieces of 1024 samples: the phasor is renewed from the double phase at each piece
    for (size_t p0 = 0; p0 < n; p0 += 1024) {
        const size_t pn = std::min<size_t>(1024, n - p0);
        // The phasors of the piece in eight independent lanes (each steps by step^8), then the mix as one plain loop: one rotation chain
        // would wait on its own latency every sample. The products are written out: std::complex<float> * calls the NaN-checking library
        // multiply (__mulsc3) without -ffast-math.
        float lr[8], li[8];
        for (int k = 0; k < 8; k++) { const double t = ph_ + w_ * k; lr[k] = (float)std::cos(t); li[k] = (float)std::sin(t); }
        const float s8r = (float)std::cos(8 * w_), s8i = (float)std::sin(8 * w_);
        for (size_t i = 0; i < pn; i += 8)
            for (int k = 0; k < 8; k++) {
                phr_[i + k] = lr[k]; phi_[i + k] = li[k];
                const float t = lr[k] * s8r - li[k] * s8i; li[k] = lr[k] * s8i + li[k] * s8r; lr[k] = t;
            }
        ph_ = std::fmod(ph_ + w_ * (double)pn, 2.0 * kPi);
        const cf32* y = x + p0;
        const float* f = reinterpret_cast<const float*>(y);
        for (size_t i = 0; i < pn; i++) {
            const float yr = f[2 * i], yi = f[2 * i + 1], pr = phr_[i], pi = phi_[i];
            mr_[i] = yr * pr - yi * pi; mi_[i] = yr * pi + yi * pr;
        }
        if (R_ == 1) {
            for (size_t i = 0; i < pn; i++) a.push_back(cf32(mr_[i], mi_[i]));
            continue;
        }
        const float sc = (float)scale_;
        // the integrators in locals for the piece (members behind a vector's push_back would be reloaded every sample)
        uint64_t i0 = ig_[0][0], i1 = ig_[0][1], i2 = ig_[0][2], i3 = ig_[0][3], q0 = ig_[1][0], q1 = ig_[1][1], q2 = ig_[1][2], q3 = ig_[1][3];
        int cnt = cnt_;
        for (size_t i = 0; i < pn; i++) {
            const float re = std::min(8.f, std::max(-8.f, mr_[i])), im = std::min(8.f, std::max(-8.f, mi_[i]));
            i0 += (uint64_t)(int64_t)(re * sc); i1 += i0; i2 += i1; i3 += i2;
            q0 += (uint64_t)(int64_t)(im * sc); q1 += q0; q2 += q1; q3 += q2;
            if (++cnt < R_) continue;
            cnt = 0;
            uint64_t ci = i3, cq = q3;
            for (int s = 0; s < 4; s++) {
                const uint64_t ti = ci - dl_[0][s], tq = cq - dl_[1][s];
                dl_[0][s] = ci; dl_[1][s] = cq;
                ci = ti; cq = tq;
            }
            a.push_back(cf32((float)((double)(int64_t)ci * outScale_), (float)((double)(int64_t)cq * outScale_)));
        }
        ig_[0][0] = i0; ig_[0][1] = i1; ig_[0][2] = i2; ig_[0][3] = i3; ig_[1][0] = q0; ig_[1][1] = q1; ig_[1][2] = q2; ig_[1][3] = q3;
        cnt_ = cnt;
    }
    if (H_ == 0) return;
    // the half-bands: y[c] = x[c] / 2 + sum_k h_k (x[c - k] + x[c + k]) over the odd k, every second c
    const float* h = hbTaps().h;
    std::vector<cf32>* in = &a_;
    for (int s = 0; s < H_; s++) {
        Hb& st = hb_[(size_t)s];
        std::vector<cf32>& dst = s + 1 == H_ ? out : (in == &a_ ? b_ : a_);
        if (&dst != &out) dst.clear();
        st.buf.insert(st.buf.end(), in->begin(), in->end());
        const cf32* b = st.buf.data();
        size_t c = st.next;
        for (; c + kHbL < st.buf.size(); c += 2) {
            float accR = 0.5f * b[c].real(), accI = 0.5f * b[c].imag();
            for (int j = 0; j < (kHbL + 1) / 2; j++) {
                const int k = 2 * j + 1;
                accR += h[j] * (b[c - (size_t)k].real() + b[c + (size_t)k].real());
                accI += h[j] * (b[c - (size_t)k].imag() + b[c + (size_t)k].imag());
            }
            dst.push_back(cf32(accR, accI));
        }
        const size_t drop = c - (size_t)kHbL;   // keep kHbL samples before the next centre
        st.buf.erase(st.buf.begin(), st.buf.begin() + (long)drop);
        st.next = c - drop;
        in = &dst;
    }
}

// ---- the analyser -----------------------------------------------------------------------------------------------------------------------
struct ZoomSpectrum::Impl {
    mutable std::mutex mu;
    zoomfft::Plan plan, accPlan;
    double fs = 0;
    uint64_t gen = 1, seq = 0;
    int window = 0;
    size_t maxBlocks = SpectrumAnalyzer::kDefaultBlocks;
    std::vector<float> accum;
    size_t blocks = 0;
    struct Worker {
        uint64_t gen = 0;
        bool on = false;
        zoomfft::Plan p;
        ZoomDecimator dec;
        std::vector<cf32> dout, ring;
        uint64_t pos = 0;
        double nextAt = 0, hop = 0;
        int log2n = 0;
        std::vector<float> win, re, im;
    } w;
};

ZoomSpectrum::ZoomSpectrum() : p_(new Impl) {}
ZoomSpectrum::~ZoomSpectrum() { delete p_; }

void ZoomSpectrum::configure(const zoomfft::Plan& p, double fs) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& s = *p_;
    if (p.on == s.plan.on && (!p.on || (p.id == s.plan.id && fs == s.fs && p.sameShape(s.plan) && p.offsetHz == s.plan.offsetHz))) return;
    s.plan = p; s.fs = fs; s.accPlan = p;
    s.accum.assign(p.on ? p.bins : 0, 0.f);
    s.blocks = 0; s.gen++;
}

zoomfft::Plan ZoomSpectrum::plan() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->plan; }

void ZoomSpectrum::setWindow(int type) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->window == type) return;
    p_->window = type; p_->gen++;
    std::fill(p_->accum.begin(), p_->accum.end(), 0.f); p_->blocks = 0;
}

void ZoomSpectrum::setBlocksPerFrame(size_t n) { std::lock_guard<std::mutex> lk(p_->mu); p_->maxBlocks = std::max<size_t>(1, std::min<size_t>(64, n)); }

void ZoomSpectrum::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::fill(p_->accum.begin(), p_->accum.end(), 0.f);
    p_->blocks = 0; p_->gen++;
}

void ZoomSpectrum::feed(const cf32* x, size_t n) {
    Impl& s = *p_;
    Impl::Worker& w = s.w;
    uint64_t gen; zoomfft::Plan p; double fs; int window; size_t maxBlocks;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        gen = s.gen; p = s.plan; fs = s.fs; window = s.window; maxBlocks = s.maxBlocks;
    }
    if (!p.on || !(fs > 0) || p.bins < 2) {
        if (w.on) { w.on = false; w.ring = std::vector<cf32>(); w.dout = std::vector<cf32>(); w.win = w.re = w.im = std::vector<float>(); }
        w.gen = gen;
        return;
    }
    const size_t N = p.bins;
    if (w.gen != gen || !w.on) {
        w.gen = gen; w.on = true; w.p = p;
        w.dec.setup(fs, p.offsetHz, p.cic, p.halfbands);
        w.ring.assign(N, cf32(0.f, 0.f));
        w.pos = 0; w.nextAt = (double)N;
        w.hop = (double)N / p.rateHz > 1.0 / 35.0 ? p.rateHz / 35.0 : (double)N;   // long transforms overlap: about 35 a second
        w.log2n = 0; while ((size_t(1) << w.log2n) < N) w.log2n++;
        w.win.resize(N); specwin::make(window, w.win.data(), (int)N);
        w.re.assign(N, 0.f); w.im.assign(N, 0.f);
    }
    w.dout.clear();
    w.dec.run(x, n, w.dout);
    size_t used = 0;
    const size_t m = w.dout.size();
    while (used < m) {
        const uint64_t endAt = (uint64_t)std::llround(w.nextAt);
        size_t take = m - used;
        if (endAt > w.pos) take = (size_t)std::min<uint64_t>(take, endAt - w.pos);
        for (size_t k = 0; k < take;) {
            const size_t at = (size_t)((w.pos + k) % N), run = std::min(take - k, N - at);
            std::copy(w.dout.begin() + (long)(used + k), w.dout.begin() + (long)(used + k + run), w.ring.begin() + (long)at);
            k += run;
        }
        w.pos += take; used += take;
        if (w.pos < endAt) continue;
        w.nextAt += w.hop;
        if (w.nextAt < (double)w.pos) w.nextAt = (double)w.pos + w.hop;
        if (w.pos < N) continue;
        {
            std::lock_guard<std::mutex> lk(s.mu);
            if (w.gen != s.gen || s.blocks >= maxBlocks) continue;   // the frame has its blocks (or the plan changed)
        }
        const size_t at = (size_t)(w.pos % N);   // the oldest sample sits where the next one will be written
        const float* wn = w.win.data(); float* re = w.re.data(); float* im = w.im.data();
        for (size_t i = 0; i < N; i++) { const cf32 v = w.ring[(at + i) & (N - 1)]; re[i] = v.real() * wn[i]; im[i] = v.imag() * wn[i]; }
        fftSplit(re, im, w.log2n, false);
        std::lock_guard<std::mutex> lk(s.mu);
        if (w.gen != s.gen || s.blocks >= maxBlocks || s.accum.size() != N) continue;
        float* acc = s.accum.data();
        for (size_t i = 0; i < N; i++) acc[i] += re[i] * re[i] + im[i] * im[i];
        s.blocks++;
    }
}

bool ZoomSpectrum::takeFrame(ZoomFrame& out) {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.plan.on || s.blocks == 0 || s.accum.empty()) return false;
    const size_t N = s.accum.size(), h = N / 2;
    out.dbfs.resize(N);
    const float inv = 1.0f / ((float)s.blocks * (float)N * (float)N);   // dBFS per bin, as SpectrumAnalyzer
    float* o = out.dbfs.data(); const float* ac = s.accum.data();
    for (size_t i = 0; i < h; i++) o[i] = spectrumDb(std::max(ac[i + h] * inv, 1e-14f));
    for (size_t i = h; i < N; i++) o[i] = spectrumDb(std::max(ac[i - h] * inv, 1e-14f));
    out.plan = s.accPlan;
    out.seq = ++s.seq;
    std::fill(s.accum.begin(), s.accum.end(), 0.f);
    s.blocks = 0;
    return true;
}

}  // namespace dect2
