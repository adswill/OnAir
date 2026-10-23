#include "dect2/dsp/synth_wide.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>

namespace dect2::synth {

static const double kPi = 3.14159265358979323846;

std::vector<float> sqrtHann(int n) {
    std::vector<float> w(n);
    for (int i = 0; i < n; i++) w[i] = (float)std::sin(kPi * i / n);   // sqrt of the periodic Hann 0.5(1-cos(2 pi i/n))
    return w;
}

float fade(int i, int n) {
    if (n <= 0) return 1.f;
    return (float)(0.5 * (1.0 - std::cos(kPi * (i + 0.5) / n)));
}

int estimateShiftBins(const std::vector<float>& pa, const std::vector<float>& pb, int maxLag, float* confidence) {
    const int w = (int)std::min(pa.size(), pb.size());
    if (confidence) *confidence = 0;
    if (w < 8) return 0;
    maxLag = std::min(maxLag, w / 2);
    double ma = 0, mb = 0;
    for (int i = 0; i < w; i++) { ma += pa[i]; mb += pb[i]; }
    ma /= w; mb /= w;
    int best = 0; double bestV = -1e300, sumAbs = 0;
    std::vector<double> c(2 * maxLag + 1);
    for (int d = -maxLag; d <= maxLag; d++) {   // pa(w) ~ pb(w - d)
        double s = 0; int cnt = 0;
        for (int i = std::max(0, d); i < std::min(w, w + d); i++) { s += (pa[i] - ma) * (pb[i - d] - mb); cnt++; }
        const double v = cnt ? s / cnt : 0;
        c[d + maxLag] = v; sumAbs += std::fabs(v);
        if (v > bestV) { bestV = v; best = d; }
    }
    const double mean = sumAbs / c.size();
    if (confidence) *confidence = mean > 0 ? (float)(bestV / mean) : 0.f;
    return best;
}

DelayPhase estimateDelay(const std::vector<cf32>& cross, int n) {
    DelayPhase r;
    const int w = (int)cross.size();
    if (w < 8 || w > n) return r;
    const int pad = 4, L = n * pad;
    Fft f(L);
    std::vector<cf32> c(L, cf32(0, 0));
    double tot = 0;
    for (int u = 0; u < w; u++) { c[u] = cross[u]; tot += std::abs(cross[u]); }
    f.forward(c.data());
    int bi = 0; float bm = -1;
    for (int i = 0; i < L; i++) { const float m = std::abs(c[i]); if (m > bm) { bm = m; bi = i; } }
    const float ml = std::abs(c[(bi + L - 1) % L]), mr = std::abs(c[(bi + 1) % L]);
    const float den = ml - 2 * bm + mr;
    double frac = den < -1e-20f ? 0.5 * (ml - mr) / den : 0.0;
    double idx = bi + frac;
    if (idx >= L / 2) idx -= L;
    r.delay = idx / pad;   // bin step n/L of the delay axis: a delay of tau samples puts the peak at tau*L/n = tau*pad
    // refine: the transform of a band-limited cross-spectrum is a sinc-like peak, the parabola above is biased by the fraction; climb the exact |c(tau)|
    auto mag = [&](double tau) {
        std::complex<double> s(0, 0);
        const double th = 2.0 * kPi * tau / n;
        const std::complex<double> rot = std::polar(1.0, -th);
        std::complex<double> ph(1.0, 0.0);
        for (int u = 0; u < w; u++) { s += std::complex<double>(cross[u].real(), cross[u].imag()) * ph; ph *= rot; }
        return std::abs(s);
    };
    double h = 0.25;
    for (int it = 0; it < 14; it++) {
        const double m0 = mag(r.delay), ml2 = mag(r.delay - h), mr2 = mag(r.delay + h);
        if (ml2 > m0 && ml2 >= mr2) r.delay -= h;
        else if (mr2 > m0) r.delay += h;
        else { const double dd = ml2 - 2 * m0 + mr2; if (dd < 0) r.delay += 0.5 * h * (ml2 - mr2) / dd; h *= 0.5; }
        if (it > 5) h *= 0.7;
    }
    // phase at the refined delay, evaluated directly
    std::complex<double> s(0, 0);
    const double th = 2.0 * kPi * r.delay / n;
    for (int u = 0; u < w; u++) s += std::complex<double>(cross[u].real(), cross[u].imag()) * std::polar(1.0, -th * u);
    r.phase0 = std::arg(s);
    r.strength = tot > 0 ? std::abs(s) / tot : 0;
    return r;
}

double fineOffsetHz(const std::vector<cf32>& hops, double hopSec) {
    std::complex<double> s(0, 0);
    for (size_t i = 1; i < hops.size(); i++)
        s += std::complex<double>(hops[i].real(), hops[i].imag()) * std::conj(std::complex<double>(hops[i - 1].real(), hops[i - 1].imag()));
    if (std::abs(s) <= 0 || hopSec <= 0) return 0;
    return std::arg(s) / (2.0 * kPi * hopSec);
}

double regressSlope(const std::vector<double>& t, const std::vector<double>& y) {
    const size_t n = std::min(t.size(), y.size());
    if (n < 2) return 0;
    double mt = 0, my = 0;
    for (size_t i = 0; i < n; i++) { mt += t[i]; my += y[i]; }
    mt /= n; my /= n;
    double num = 0, den = 0;
    for (size_t i = 0; i < n; i++) { num += (t[i] - mt) * (y[i] - my); den += (t[i] - mt) * (t[i] - mt); }
    return den > 0 ? num / den : 0;
}

double mismatchDb(const std::vector<cf32>& cross, const std::vector<float>& pa, const std::vector<float>& pb, int n, const DelayPhase& dp, double gain) {
    double sa = 0, sb = 0;
    for (float v : pa) sa += v;
    for (float v : pb) sb += v;
    std::complex<double> s(0, 0);
    for (size_t u = 0; u < cross.size(); u++)
        s += std::complex<double>(cross[u].real(), cross[u].imag()) * std::polar(1.0, -(dp.phase0 + 2.0 * kPi * dp.delay * (double)u / n));
    const double e = sa + gain * gain * sb - 2.0 * gain * s.real();
    return 10.0 * std::log10(std::max(e, 1e-30 * (sa + 1e-30)) / std::max(sa, 1e-30));
}

// ---------------------------------------------------------------------------------------------------------------------------------------------

namespace {
constexpr int kTaps = 24, kPhases = 256, kHalf = kTaps / 2;   // taps i-11 .. i+12
struct Kernel {
    std::vector<float> h;   // (kPhases + 1) x kTaps
    Kernel() : h((size_t)(kPhases + 1) * kTaps) {
        auto i0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 30; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
        const double beta = 7.5, norm = i0(beta);
        for (int ph = 0; ph <= kPhases; ph++) {
            const double mu = (double)ph / kPhases;
            for (int k = 0; k < kTaps; k++) {
                const double t = (k - (kHalf - 1)) - mu;   // tap index relative to i, minus the fraction
                const double sc = std::fabs(t) < 1e-12 ? 1.0 : std::sin(kPi * t) / (kPi * t);
                const double r = t / kHalf;
                const double w = std::fabs(r) >= 1 ? 0.0 : i0(beta * std::sqrt(1 - r * r)) / norm;
                h[(size_t)ph * kTaps + k] = (float)(sc * w);
            }
        }
    }
};
}

void FracResampler::process(const cf32* x, size_t n, std::vector<cf32>& out) {
    static const Kernel K;
    buf_.insert(buf_.end(), x, x + n);
    const double step = 1.0 + ppm_ * 1e-6;
    const double dph = 2.0 * kPi * off_ / fs_;
    const std::complex<double> rot = std::polar(1.0, dph);
    std::complex<double> ph = std::polar(1.0, ph_);
    while ((size_t)pos_ + kHalf < buf_.size()) {
        const size_t i = (size_t)pos_;
        const double mu = pos_ - (double)i;
        const double fp = mu * kPhases;
        const int pi = (int)fp;
        const float fr = (float)(fp - pi);
        const float* h0 = &K.h[(size_t)pi * kTaps];
        const float* h1 = h0 + kTaps;
        const cf32* src = &buf_[i - (kHalf - 1)];
        float re = 0, im = 0;
        for (int k = 0; k < kTaps; k++) {
            const float c = h0[k] + (h1[k] - h0[k]) * fr;
            re += src[k].real() * c; im += src[k].imag() * c;
        }
        out.emplace_back(cf32(re, im) * cf32((float)ph.real(), (float)ph.imag()));
        ph *= rot;
        pos_ += step;
    }
    ph_ = std::arg(ph);
    const size_t keep = (size_t)pos_ >= (size_t)(kHalf - 1) ? (size_t)pos_ - (kHalf - 1) : 0;
    if (keep > 0) { buf_.erase(buf_.begin(), buf_.begin() + keep); pos_ -= (double)keep; }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------

bool SynthWideband::configure(const SynthCfg& c) {
    if (c.slices < 2 || c.slices > kSynthMaxSlices) return false;
    if (c.n < 256 || (c.n & (c.n - 1))) return false;
    if (c.stepBins <= 0 || (c.stepBins & 1)) return false;
    int u = (int)(c.usable * c.n);
    u &= ~1;
    if (u < 64 || u >= c.n) return false;
    const int ov = u - c.stepBins;
    if (ov < 64) return false;
    c_ = c;
    usableBins_ = u; ov_ = ov;
    const int span = (c.slices - 1) * c.stepBins + u;
    m_ = c.n;
    while (m_ < span) m_ *= 2;
    r_.assign(c.slices, Radio());
    for (auto& r : r_) { r.rs.configure(c.fs); r.spec.assign(c.n, cf32(0, 0)); }
    acc_.assign(c.slices - 1, Acc());
    for (auto& a : acc_) a.clear(ov);
    trk_.assign(c.slices - 1, Track());
    pairs_.assign(c.slices - 1, PairStatus());
    sl_.assign(c.slices, SliceStatus());
    amp_.assign(c.slices, 1.0); ph_.assign(c.slices, 0.0); tau_.assign(c.slices, 0.0);
    tH_.assign(c.slices - 1, {}); dH_.assign(c.slices - 1, {});
    shifted_.assign(c.slices, 0.0);
    stage_.assign(c.slices - 1, 0);
    lastMismatch_.assign(c.slices - 1, 0.f);
    offRate_.assign(c.slices, 0.0); lastOff_.assign(c.slices, 0.0); lastOffT_.assign(c.slices, -1.0);
    win_ = sqrtHann(c.n);
    wwin_ = sqrtHann(m_);
    fftN_ = std::make_unique<Fft>(c.n);
    fftM_ = std::make_unique<Fft>(m_);
    olap_.assign(m_, cf32(0, 0));
    outQ_.clear();
    spAvg_.assign(m_, 0.f);
    t_ = 0;
    rebuildWeights();
    return true;
}

void SynthWideband::rebuildWeights() {
    const int S = c_.slices, U = usableBins_;
    wt_.assign(S, std::vector<float>(U, 1.f));
    for (int i = 0; i < S; i++) {
        const bool left = i > 0 && r_[i - 1].alive, right = i + 1 < S && r_[i + 1].alive;
        auto& w = wt_[i];
        if (!r_[i].alive) { std::fill(w.begin(), w.end(), 0.f); continue; }
        const int le = left ? ov_ : 16, re = right ? ov_ : 16;
        for (int j = 0; j < le && j < U; j++) w[j] = fade(j, le);
        for (int j = 0; j < re && j < U; j++) w[U - re + j] = std::min(w[U - re + j], fade(re - 1 - j, re));
    }
}

void SynthWideband::setAlive(int i, bool alive) {
    if (i < 0 || i >= c_.slices) return;
    if (alive) {
        if (r_[i].alive) return;
        r_[i].alive = true; r_[i].manualDown = false;
        r_[i].ring.clear(); r_[i].head = 0; r_[i].rs.reset(); r_[i].pendDrop = 0; shifted_[i] = 0;
        for (int p = std::max(0, i - 1); p <= std::min(i, c_.slices - 2); p++) { acc_[p].clear(ov_); stage_[p] = 0; tH_[p].clear(); dH_[p].clear(); trk_[p] = Track(); }
        // the others drop what they hold, so the returning radio and the rest are about equally current
        for (int k = 0; k < c_.slices; k++) if (k != i && r_[k].alive) r_[k].head = r_[k].ring.size();
        sl_[i].alive = true; sl_[i].locked = false;
        rebuildWeights();
    } else {
        r_[i].manualDown = true;
        markDown(i);
    }
}

void SynthWideband::markDown(int i) {
    if (!r_[i].alive) return;
    r_[i].alive = false;
    r_[i].ring.clear(); r_[i].head = 0;
    sl_[i].alive = false; sl_[i].locked = false; sl_[i].dropouts++;
    for (int p = std::max(0, i - 1); p <= std::min(i, c_.slices - 2); p++) { pairs_[p].valid = false; pairs_[p].tracking = false; acc_[p].clear(ov_); trk_[p] = Track(); }
    rebuildWeights();
}

void SynthWideband::push(int i, const cf32* x, size_t n) {
    if (i < 0 || i >= c_.slices || n == 0) return;
    Radio& r = r_[i];
    if (!r.alive) {
        if (r.manualDown) return;
        setAlive(i, true);   // a radio that fell behind and is delivering again
    }
    r.rs.process(x, n, r.ring);
    if (r.pendDrop > 0) {
        const size_t have = r.ring.size() - r.head;
        const size_t d = std::min<size_t>((size_t)r.pendDrop, have);
        r.ring.erase(r.ring.begin() + r.head, r.ring.begin() + r.head + d);
        r.pendDrop -= (int)d;
    }
    run();
}

bool SynthWideband::haveFrame(int i) const { return r_[i].ring.size() - r_[i].head >= (size_t)c_.n; }

void SynthWideband::run() {
    for (;;) {
        int alive = 0, ready = 0;
        size_t lead = 0;
        for (int i = 0; i < c_.slices; i++) {
            if (!r_[i].alive) continue;
            alive++;
            if (haveFrame(i)) ready++;
            lead = std::max(lead, r_[i].ring.size() - r_[i].head);
        }
        if (alive == 0) return;
        if (ready < alive) {
            if (lead > (size_t)c_.n * 24 && ready > 0) {   // somebody stalled far behind: leave it out until it delivers again
                bool any = false;
                for (int i = 0; i < c_.slices; i++) if (r_[i].alive && !haveFrame(i) && alive - 1 >= 1) { markDown(i); any = true; break; }
                if (any) continue;
            }
            return;
        }
        hop();
    }
}

std::vector<float> SynthWideband::spectrumDb() const {
    std::vector<float> o(m_);
    const double sc = 0.405 * c_.n * (double)c_.n;
    for (int k = 0; k < m_; k++) {
        const int idx = (k + m_ / 2) % m_;   // negative frequencies first
        o[k] = (float)(10.0 * std::log10(spAvg_[idx] / sc + 1e-20));
    }
    return o;
}

void SynthWideband::hop() {
    const int N = c_.n, S = c_.slices, U = usableBins_, M = m_;
    std::vector<cf32> tmp(N);
    for (int i = 0; i < S; i++) {
        Radio& r = r_[i];
        if (!r.alive) continue;
        for (int k = 0; k < N; k++) tmp[k] = r.ring[r.head + k] * win_[k];
        fftN_->forward(tmp.data());
        for (int j = -N / 2; j < N / 2; j++) r.spec[j + N / 2] = tmp[(j + N) % N];
    }
    if (c_.trackPhase) trackHop();
    // alignment statistics: pair p (slices p, p+1): wide bins [k_{p+1}-U/2, k_p+U/2)
    for (int p = 0; p + 1 < S; p++) {
        if (!r_[p].alive || !r_[p + 1].alive) continue;
        Acc& a = acc_[p];
        const int ja = c_.stepBins;   // local index (0..U-1 offset) of the overlap start in slice p's usable range
        const int offA = N / 2 - U / 2 + ja, offB = N / 2 - U / 2;
        const size_t base = a.rows.size();
        a.rows.resize(base + ov_);
        const bool trk = c_.trackPhase && trk_[p].init;
        const cf32 deph = trk ? cf32((float)std::cos(trk_[p].phi), (float)-std::sin(trk_[p].phi)) : cf32(1, 0);
        for (int u = 0; u < ov_; u++) {
            const cf32 A = r_[p].spec[offA + u], B = r_[p + 1].spec[offB + u];
            const cf32 cr = A * std::conj(B);
            a.cross[u] += cr; a.pa[u] += std::norm(A); a.pb[u] += std::norm(B);
            a.rows[base + u] = cr;
            if (trk) a.crossT[u] += cr * deph;
        }
        a.frames++;
        if (trk) a.trkFrames++;
    }
    // stitch
    std::vector<cf32> Y(M, cf32(0, 0));
    for (int i = 0; i < S; i++) {
        if (!r_[i].alive) continue;
        const int k = (int)std::lround((i - 0.5 * (S - 1)) * c_.stepBins);
        const double st = 2.0 * kPi * tau_[i] / N;
        const int w0 = k - U / 2;
        std::complex<double> rot = std::polar(1.0, st), ph = std::polar(amp_[i], ph_[i] + st * w0);
        const int off = N / 2 - U / 2;
        for (int u = 0; u < U; u++) {
            const float wt = wt_[i][u];
            if (wt > 0.f) {
                const cf32 g((float)ph.real(), (float)ph.imag());
                const int w = w0 + u;
                Y[(w % M + M) % M] += r_[i].spec[off + u] * (g * wt);
            }
            ph *= rot;
        }
    }
    const float sc = 1.f / (float)N;
    for (int k = 0; k < M; k++) {
        const float p = std::norm(Y[k]);
        spAvg_[k] += 0.1f * (p - spAvg_[k]);
    }
    fftM_->inverse(Y.data());
    for (int k = 0; k < M; k++) olap_[k] += Y[k] * (sc * wwin_[k]);
    outQ_.insert(outQ_.end(), olap_.begin(), olap_.begin() + M / 2);
    std::copy(olap_.begin() + M / 2, olap_.end(), olap_.begin());
    std::fill(olap_.begin() + M / 2, olap_.end(), cf32(0, 0));
    // consume
    for (int i = 0; i < S; i++) {
        Radio& r = r_[i];
        if (!r.alive) continue;
        r.head += N / 2;
        if (r.head > (size_t)N * 8) { r.ring.erase(r.ring.begin(), r.ring.begin() + r.head); r.head = 0; }
    }
    t_ += (double)(N / 2) / c_.fs;
    // a slice cut off from its left neighbour: its frequency correction follows the drift rate measured before
    for (int i = 1; i < S; i++) {
        Radio& r = r_[i];
        if (!r.alive || r_[i - 1].alive || offRate_[i] == 0.0) continue;
        r.off += offRate_[i] * (double)(N / 2) / c_.fs;
        r.rs.set(r.ppm, r.off);
        sl_[i].offsetHz = r.off;
    }
    for (int p = 0; p + 1 < S; p++)
        if (r_[p].alive && r_[p + 1].alive && acc_[p].frames >= c_.alignFrames) solve(p);
}

size_t SynthWideband::pull(std::vector<cf32>& out) {
    const size_t n = outQ_.size();
    out.insert(out.end(), outQ_.begin(), outQ_.end());
    outQ_.clear();
    return n;
}

void SynthWideband::shiftSlice(int i, int d) {
    Radio& r = r_[i];
    if (d > 0) {
        const size_t have = r.ring.size() - r.head;
        const size_t k = std::min<size_t>((size_t)d, have);
        r.ring.erase(r.ring.begin() + r.head, r.ring.begin() + r.head + k);
        r.pendDrop += d - (int)k;
    } else if (d < 0) {
        r.ring.insert(r.ring.begin() + r.head, (size_t)(-d), cf32(0, 0));
    }
    shifted_[i] += d;
    for (int p = std::max(0, i - 1); p <= std::min(i, c_.slices - 2); p++) { acc_[p].clear(ov_); tH_[p].clear(); dH_[p].clear(); trk_[p] = Track(); pairs_[p].tracking = false; }
}

void SynthWideband::recalibrate() {
    for (int i = 0; i < c_.slices; i++) {
        Radio& r = r_[i];
        r.off = 0; r.ppm = 0; r.rs.set(0, 0); r.pendDrop = 0;
        shifted_[i] = 0;
        amp_[i] = 1; ph_[i] = 0; tau_[i] = 0;
        sl_[i].locked = false; sl_[i].offsetHz = 0; sl_[i].driftPpm = 0;
        offRate_[i] = 0; lastOffT_[i] = -1;
    }
    for (int p = 0; p + 1 < c_.slices; p++) {
        acc_[p].clear(ov_); trk_[p] = Track(); stage_[p] = 0; tH_[p].clear(); dH_[p].clear();
        pairs_[p] = PairStatus();
    }
}

void SynthWideband::trackHop() {
    // every pair in order from slice 0: measure this hop's seam phase, filter it, and set the slice's phase correction for this very hop
    const int N = c_.n, S = c_.slices, U = usableBins_;
    const double hopSec = (double)(N / 2) / c_.fs;
    const double q = 2.0 * kPi * std::max(0.1, c_.trackLinewidthHz) * hopSec;   // phase random-walk variance per hop (Lorentzian linewidth)
    const double qw = q * 1e-3;                                                // the phase step per hop (residual frequency) moves slowly
    for (int p = 0; p + 1 < S; p++) {
        Track& t = trk_[p];
        if (!t.init || !r_[p].alive || !r_[p + 1].alive) continue;
        const int offA = N / 2 - U / 2 + c_.stepBins, offB = N / 2 - U / 2;
        std::complex<double> z(0, 0);
        double mag = 0;
        const std::complex<double> rot = std::polar(1.0, -2.0 * kPi * t.delay / N);
        std::complex<double> ph(1.0, 0.0);
        for (int u = 0; u < ov_; u++) {
            const cf32 A = r_[p].spec[offA + u], B = r_[p + 1].spec[offB + u];
            const cf32 cr = A * std::conj(B);
            z += std::complex<double>(cr.real(), cr.imag()) * ph;
            mag += std::abs(cr);
            ph *= rot;
        }
        // predict
        t.phi += t.w;
        t.p00 += 2 * t.p01 + t.p11 + q; t.p01 += t.p11; t.p11 += qw;
        if (mag > 0 && std::abs(z) > 0) {
            const double c = std::min(0.999999, std::abs(z) / mag);   // coherence of the common signal in this hop's overlap
            const double R = 2.0 * (1.0 - c) / (c * ov_) + 1e-7;
            const double innov = std::remainder(std::arg(z) - t.phi, 2.0 * kPi);
            const double S2 = t.p00 + R, k0 = t.p00 / S2, k1 = t.p01 / S2;
            t.phi += k0 * innov; t.w += k1 * innov;
            const double p00 = (1 - k0) * t.p00, p01 = (1 - k0) * t.p01, p11 = t.p11 - k1 * t.p01;
            t.p00 = p00; t.p01 = p01; t.p11 = p11;
            t.sq += (k0 * innov) * (k0 * innov); t.nsq++;
            if (t.nsq >= 256) { pairs_[p].trackRmsDeg = std::sqrt(t.sq / t.nsq) * 180.0 / kPi; t.sq = 0; t.nsq = 0; }
        }
        t.phi = std::remainder(t.phi, 2.0 * kPi);
        const int b = p + 1;
        const double w0 = (double)((int)std::lround((b - 0.5 * (S - 1)) * c_.stepBins) - U / 2);
        ph_[b] = ph_[p] + t.phi - 2.0 * kPi * t.delay * w0 / N;
    }
}

void SynthWideband::solve(int p) {
    Acc& a = acc_[p];
    const int N = c_.n, U = usableBins_, b = p + 1;
    const double df = c_.fs / N, hopSec = (double)(N / 2) / c_.fs;
    Radio& rb = r_[b];
    PairStatus& ps = pairs_[p];
    auto applyRs = [&]() { rb.rs.set(rb.ppm, rb.off); sl_[b].offsetHz = rb.off; sl_[b].driftPpm = rb.ppm; };
    // coarse frequency offset (needs spectral structure in the overlap)
    if (stage_[p] == 0) {
        float conf = 0;
        const int lag = std::min(ov_ / 2 - 1, (int)std::ceil(c_.maxOffsetHz / df));
        const int d = estimateShiftBins(a.pa, a.pb, lag, &conf);
        if (d != 0 && conf >= 3.f) {
            rb.off += d * df;
            applyRs();
            a.clear(ov_);
            return;
        }
        stage_[p] = 1;
    }
    // delay / phase / gain from the accumulated cross-spectrum (with the tracked phase taken out when the tracker ran the whole time: the
    // free-running phase wander then no longer smears the average)
    const bool tracked = c_.trackPhase && trk_[p].init && a.trkFrames == a.frames && a.frames > 0;
    DelayPhase dp = estimateDelay(tracked ? a.crossT : a.cross, N);
    if (std::fabs(dp.delay) > 8.0 && dp.strength > 0.05) {
        shiftSlice(b, (int)std::lround(dp.delay));
        return;
    }
    // fine frequency offset: the per-hop cross sums with the delay slope taken out
    {
        std::vector<cf32> hops(a.frames);
        for (int t = 0; t < a.frames; t++) {
            std::complex<double> s(0, 0);
            const double th = 2.0 * kPi * dp.delay / N;
            for (int u = 0; u < ov_; u++) {
                const cf32 v = a.rows[(size_t)t * ov_ + u];
                s += std::complex<double>(v.real(), v.imag()) * std::polar(1.0, -th * u);
            }
            hops[t] = cf32((float)s.real(), (float)s.imag());
        }
        const double dh = fineOffsetHz(hops, hopSec);
        if (std::fabs(dh) > 0.5) {
            rb.off += 0.7 * dh;
            applyRs();
            if (std::fabs(dh) > 3.0) { a.clear(ov_); stage_[p] = 1; return; }   // measure again on the corrected stream
        }
        if (stage_[p] == 1) stage_[p] = 2;
    }
    double sa = 0, sb = 0;
    for (float v : a.pa) sa += v;
    for (float v : a.pb) sb += v;
    if (sa <= 0 || sb <= 0) { a.clear(ov_); return; }
    const double r = std::sqrt(sa / sb);
    const double w0 = (double)((int)std::lround((b - 0.5 * (c_.slices - 1)) * c_.stepBins) - U / 2);
    amp_[b] = amp_[p] * r;
    tau_[b] = tau_[p] + dp.delay;
    if (c_.trackPhase && (trk_[p].init || stage_[p] == 2)) {
        Track& t = trk_[p];
        // the tracker measures the absolute seam phase every hop, so the block average only starts it (adding the block's mean residual
        // back as a correction would fight the per-hop measurement)
        if (!t.init) { t = Track(); t.init = true; t.phi = dp.phase0; t.p00 = 0.1; t.p11 = 1e-3; }
        t.delay = dp.delay;
        ps.tracking = true;
        ph_[b] = ph_[p] + t.phi - 2.0 * kPi * dp.delay * w0 / N;
    } else {
        ph_[b] = ph_[p] + dp.phase0 - 2.0 * kPi * dp.delay * w0 / N;
    }
    ps.valid = true;
    ps.gainDb = 20.0 * std::log10(r);
    ps.delaySamples = dp.delay;
    ps.confidence = dp.strength;
    ps.mismatchDb = mismatchDb(tracked ? a.crossT : a.cross, a.pa, a.pb, N, dp, r);
    // clock drift: slope of the total delay seen over time -> back into the resampler
    tH_[p].push_back(t_);
    dH_[p].push_back(shifted_[b] + dp.delay);
    if (tH_[p].size() >= 8) {
        const double slope = regressSlope(tH_[p], dH_[p]);   // samples per second
        const double ppm = slope / c_.fs * 1e6;
        rb.ppm += 0.4 * ppm;
        applyRs();
        tH_[p].clear(); dH_[p].clear();
    }
    sl_[b].locked = stage_[p] == 2 && ps.mismatchDb < -15.0 && dp.strength > 0.3;
    // the drift rate of the frequency correction, from locked solves at least half a second apart
    if (sl_[b].locked) {
        if (lastOffT_[b] < 0) { lastOffT_[b] = t_; lastOff_[b] = rb.off; }
        else if (t_ - lastOffT_[b] >= 0.5) {
            const double rate = (rb.off - lastOff_[b]) / (t_ - lastOffT_[b]);
            if (std::fabs(rate) < 50.0) offRate_[b] = 0.7 * offRate_[b] + 0.3 * rate;
            lastOffT_[b] = t_; lastOff_[b] = rb.off;
        }
    } else {
        lastOffT_[b] = -1;
    }
    a.clear(ov_);
}

} // namespace dect2::synth
