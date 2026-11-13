// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Modulation quality (MER / EVM) meter for a blind M-PSK carrier (BPSK, QPSK, 8-PSK; 300 baud up to about a third of the working rate, i.e. 40 kBd at the default 250 ksps).
// Every 2 seconds of the channel: the symbol rate from the cyclic line of the squared envelope, the order M and the carrier from the M-th power line (smallest of 2, 4, 8 with a
// clear line; the alias of a wrapped line is resolved with the spectral centroid), a root-raised-cosine matched filter (roll-off 0.2, 0.35 or 0.5: the best reading is kept),
// Gardner timing, a decision-directed phase loop, and then MER = signal power over error power of the symbols against their nearest ideal points (EVM is the same number as a
// percentage of the RMS amplitude). Limits: hard decisions flatter the reading below about 8 dB MER (the error is clipped at half a decision distance), so a low value
// says "poor", not how poor; QAM, APSK, FSK and OFDM are not measured; the carrier must be alone in the 90 kHz of the channel. A reading of this signal as this radio receives it
// (including its phase noise and clock), not a type-approval measurement. Receive only.
#include "x40_dsp.h"
#include <complex>
#include <cstdlib>

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 100000, kCut = 45000, kMinBaud = 300, kBlockSec = 2.0, kRateRatio = 8.0, kMinMer = 4.0;

inline cf32 cpow(cf32 z, int m) { cf32 r = z; for (int i = 1; i < m; i++) r *= z; return r; }

struct Meas { bool ok = false; int M = 0; double mer = 0, evm = 0, baud = 0, fcHz = 0, alpha = 0; int nSym = 0; };

class EvmDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, kCut)) return false;
        rate_ = dec_.outRate();
        blockN_ = (size_t)(kBlockSec * rate_);
        nf_ = 1; while (nf_ < blockN_) nf_ *= 2;
        reset(); return true;
    }
    void reset() override { dec_.reset(); buf_.clear(); skip_ = (size_t)(0.1 * rate_); have_ = false; locked_ = false; lastLogT_ = -1e9; lastMer_ = -99; lastM_ = 0; t_ = 0; meas_ = Meas(); nNo_ = 0; }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) {
            t_ += 1.0 / rate_;
            if (skip_) { skip_--; return; }
            buf_.push_back(a);
            if (buf_.size() >= blockN_) { analyse(); buf_.clear(); }
        });
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        if (!locked_) { t.headline = "no PSK carrier found (needs a carrier with a clear symbol rate)"; return; }
        t.headline = fmt("%s %.2f kBd: MER %.1f dB, EVM %.1f %%", name(meas_.M), meas_.baud / 1000, meas_.mer, meas_.evm);
        t.facts.push_back({"modulation", fmt("%s (M-th power line of order %d)", name(meas_.M), meas_.M)});
        t.facts.push_back({"MER", fmt("%.1f dB", meas_.mer)});
        t.facts.push_back({"EVM", fmt("%.1f %% of the RMS amplitude", meas_.evm)});
        t.facts.push_back({"symbol rate", fmt("%.2f Bd", meas_.baud)});
        t.facts.push_back({"carrier offset", fmt("%+.1f Hz from the tuned frequency", meas_.fcHz)});
        t.facts.push_back({"filter roll-off", fmt("%.2f (the best of 0.20, 0.35, 0.50)", meas_.alpha)});
        t.facts.push_back({"symbols measured", fmt("%d in the last %.0f s", meas_.nSym, kBlockSec)});
        t.facts.push_back({"note", "hard decisions: below about 8 dB the reading is too good"});
    }
private:
    static const char* name(int m) { return m == 2 ? "BPSK" : m == 4 ? "QPSK" : m == 8 ? "8-PSK" : "?"; }

    // peak of a one-sided search in a power spectrum (bins) with parabolic refinement of the log power; returns the fractional bin
    static double refine(const std::vector<float>& p, int k) {
        const int n = (int)p.size();
        const double a = std::log(p[(k + n - 1) % n] + 1e-30), b = std::log(p[k] + 1e-30), c = std::log(p[(k + 1) % n] + 1e-30);
        const double den = a - 2 * b + c;
        return k + (den < -1e-12 ? 0.5 * (a - c) / den : 0.0);
    }
    std::vector<float> spectrum(const std::vector<cf32>& z) const {      // Hann window, zero padded to nf_
        std::vector<cf32> a(nf_, cf32(0, 0)); const size_t N = z.size();
        for (size_t i = 0; i < N; i++) a[i] = z[i] * (float)(0.5 - 0.5 * std::cos(2 * kPi * i / N));
        fftx(a);
        std::vector<float> p(nf_); for (size_t i = 0; i < nf_; i++) p[i] = std::norm(a[i]);
        return p;
    }
    static float median(std::vector<float> v) { if (v.empty()) return 1e-30f; std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return std::max(v[v.size() / 2], 1e-30f); }
    double binHz() const { return rate_ / (double)nf_; }
    double wrapHz(double f) const { while (f > rate_ / 2) f -= rate_; while (f <= -rate_ / 2) f += rate_; return f; }

    void analyse() {
        have_ = true;
        const size_t N = buf_.size();
        std::vector<cf32> x(buf_);
        double pw = 0; for (auto& v : x) pw += std::norm(v);
        pw /= (double)N; if (pw < 1e-14) { locked_ = false; out->bad(); return; }
        const float sc = (float)(1.0 / std::sqrt(pw)); for (auto& v : x) v *= sc;
        out->energy();
        // 1. the spectral centroid (a rough carrier)
        const auto P = spectrum(x); const float med = median(P);
        double sw = 0, sf = 0;
        {   // centroid of the smoothed spectrum above its noise: boxcar of 64 bins, weights above 1.5 x the median
            const int W = 64; std::vector<double> cs(nf_ + 1, 0.0); for (size_t k = 0; k < nf_; k++) cs[k + 1] = cs[k] + P[k];
            for (size_t k = 0; k < nf_; k++) {
                const size_t a = k >= (size_t)W / 2 ? k - W / 2 : 0, b = std::min(nf_, k + W / 2);
                const double w = std::max(0.0, (cs[b] - cs[a]) / (double)(b - a) - 1.5 * med); if (w <= 0) continue;
                const double f = (k < nf_ / 2 ? (double)k : (double)k - (double)nf_) * binHz(); sw += w; sf += w * f;
            }
        }
        if (sw <= 0) { locked_ = false; out->bad(); return; }
        const double fc0 = sf / sw;
        // 2. symbol rate candidates: the lines of the squared envelope (a weak line, so the strongest three are all tried and the best reading wins)
        std::vector<cf32> e(N); double me = 0; for (size_t i = 0; i < N; i++) { const float v = std::norm(x[i]); e[i] = cf32(v, 0); me += v; } me /= (double)N;
        for (auto& v : e) v -= cf32((float)me, 0);
        const auto E = spectrum(e);
        const int kLo = std::max(2, (int)(kMinBaud / binHz())), kHi = (int)(rate_ / 3.0 / binHz());
        std::vector<float> band(E.begin() + kLo, E.begin() + kHi); const float medE = median(band);
        std::vector<int> cand;
        for (int pass = 0; pass < 3; pass++) {
            int kb = -1; for (int k = kLo; k < kHi; k++) { bool close = false; for (int c : cand) if (std::abs(k - c) < 20) close = true; if (!close && (kb < 0 || E[k] > E[kb])) kb = k; }
            if (kb < 0 || E[kb] < (float)kRateRatio * medE) break;
            cand.push_back(kb);
        }
        if (cand.empty()) { locked_ = false; out->bad(); return; }
        // 3. the M-th power spectra (once per block)
        std::vector<float> Zs[3]; float mz[3] = {1, 1, 1};
        const int Ms[3] = {2, 4, 8};
        for (int mi = 0; mi < 3; mi++) { std::vector<cf32> z(N); for (size_t i = 0; i < N; i++) z[i] = cpow(x[i], Ms[mi]); Zs[mi] = spectrum(z); mz[mi] = median(Zs[mi]); }
        Meas best; best.mer = -1e9;
        for (int kc : cand) {
            const double baud = refine(E, kc) * binHz();
            int M = 0; double fc = 0;
            for (int mi = 0; mi < 3; mi++) {
                const int m = Ms[mi]; const auto& Z = Zs[mi];
                const double ex = wrapHz(m * fc0); const int ek = (int)std::lround((ex < 0 ? ex + rate_ : ex) / binHz());
                const int hw = std::max(4, (int)(m * 0.12 * baud / binHz()));
                int kp = -1; float pk = 0;
                for (int d = -hw; d <= hw; d++) { const int k = ((ek + d) % (int)nf_ + (int)nf_) % (int)nf_; if (Z[k] > pk) { pk = Z[k]; kp = k; } }
                // a line stands far above its own surroundings (the spectrum of x^M is not flat, so the median of the whole band would call a hump a line)
                double nb = 0; int nn = 0;
                if (kp >= 0) for (int d = -hw; d <= hw; d++) { if (std::abs(d + ek - kp) <= 5) continue; nb += Z[((ek + d) % (int)nf_ + (int)nf_) % (int)nf_]; nn++; }
                if (kp >= 0 && nn > 0 && pk > 25.0 * nb / nn) {
                    M = m;
                    double fl = refine(Z, kp) * binHz(); if (fl > rate_ / 2) fl -= rate_;
                    fc = fc0 + wrapHz(fl - ex) / m;
                    break;
                }
            }
            (void)mz;
            if (!M) continue;
            {   // an unmodulated carrier has a line at every "rate" and a perfect MER: a modulated one spreads its power over about a symbol rate
                double inner = 0, outer = 0;
                for (size_t k = 0; k < nf_; k++) {
                    double f = (k < nf_ / 2 ? (double)k : (double)k - (double)nf_) * binHz() - fc; f = std::fabs(wrapHz(f));
                    const double w = std::max(0.0, (double)P[k] - med);
                    if (f < 1.0 * baud) { outer += w; if (f < 0.2 * baud) inner += w; }
                }
                if (outer <= 0 || inner > 0.7 * outer) continue;
            }
            // 4. mix down, bring the rate to 3 .. 16 samples per symbol
            std::vector<cf32> y(N); { double p = 0; const double dp = -2 * kPi * fc / rate_; for (size_t i = 0; i < N; i++) { y[i] = x[i] * cf32((float)std::cos(p), (float)std::sin(p)); p += dp; if (p < -kPi) p += 2 * kPi; } }
            const int q = std::max(1, (int)std::floor(rate_ / (8.0 * baud)));
            std::vector<cf32> r; r.reserve(N / q + 1);
            for (size_t i = 0; i + q <= N; i += q) { cf32 s(0, 0); for (int j = 0; j < q; j++) s += y[i + j]; r.push_back(s / (float)q); }
            const double rate2 = rate_ / q, sps = rate2 / baud;
            // 5. the matched filter for each roll-off; the best reading is kept
            for (double al : {0.2, 0.35, 0.5}) {
                Meas m = measure(r, sps, al, M);
                if (m.ok && m.mer > best.mer) { best = m; best.alpha = al; best.M = M; best.baud = baud; best.fcHz = fc; }
            }
        }
        if (!best.ok || best.mer < kMinMer) { locked_ = false; out->bad(); return; }
        const int M = best.M; const double baud = best.baud, fc = best.fcHz;
        meas_ = best; locked_ = true;
        out->good((float)best.mer, fc);
        const bool changed = M != lastM_ || std::fabs(best.mer - lastMer_) > 3.0 || t_ - lastLogT_ > 30.0;
        if (changed) {
            out->row("MER", name(M), fmt("MER %.1f dB, EVM %.1f %%, %.2f kBd, carrier %+.1f Hz", best.mer, best.evm, baud / 1000, fc), (float)toDb((float)pw));
            lastLogT_ = t_; lastMer_ = best.mer; lastM_ = M;
        }
    }

    static cf32 interp(const std::vector<cf32>& y, double pos) {            // Catmull-Rom cubic
        const long i = (long)std::floor(pos); const float u = (float)(pos - (double)i);
        const cf32 p0 = y[(size_t)(i - 1)], p1 = y[(size_t)i], p2 = y[(size_t)(i + 1)], p3 = y[(size_t)(i + 2)];
        const cf32 a1 = -p0 + p2, a2 = 2.f * p0 - 5.f * p1 + 4.f * p2 - p3, a3 = -p0 + 3.f * p1 - 3.f * p2 + p3;
        return 0.5f * (2.f * p1 + a1 * u + a2 * (u * u) + a3 * (u * u * u));
    }

    Meas measure(const std::vector<cf32>& r, double sps, double alpha, int M) const {
        Meas out_;
        // root-raised-cosine taps over +-3 symbols at the sample rate
        const int half = (int)std::ceil(3.0 * sps); std::vector<float> h(2 * half + 1); double en = 0;
        for (int k = -half; k <= half; k++) {
            const double t = k / sps; double v;
            if (k == 0) v = 1 - alpha + 4 * alpha / kPi;
            else if (std::fabs(std::fabs(4 * alpha * t) - 1) < 1e-9) v = alpha / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * alpha)) + (1 - 2 / kPi) * std::cos(kPi / (4 * alpha)));
            else v = (std::sin(kPi * t * (1 - alpha)) + 4 * alpha * t * std::cos(kPi * t * (1 + alpha))) / (kPi * t * (1 - (4 * alpha * t) * (4 * alpha * t)));
            h[k + half] = (float)v; en += v * v;
        }
        for (auto& v : h) v /= (float)std::sqrt(en);
        const size_t n = r.size(); if (n < (size_t)(8 * half)) return out_;
        std::vector<cf32> y(n, cf32(0, 0));
        for (size_t i = (size_t)half; i + half < n; i++) { cf32 a(0, 0); for (int k = -half; k <= half; k++) a += r[i + k] * h[k + half]; y[i] = a; }
        // coarse timing: the phase with the most energy at the symbol instants
        const size_t i0 = (size_t)(2 * half), i1 = n - (size_t)(2 * half) - 4;
        double bestE = -1, tau0 = 0;
        for (int s = 0; s < 16; s++) {
            const double tau = (double)i0 + sps * s / 16.0; double e = 0; int c = 0;
            for (double p = tau; p < tau + 300 * sps && p < (double)i1; p += sps) { e += std::norm(interp(y, p)); c++; }
            if (c && e / c > bestE) { bestE = e / c; tau0 = tau; }
        }
        // Gardner loop
        std::vector<cf32> sym; sym.reserve((size_t)(n / sps) + 2);
        double pos = tau0, integ = 0, pw = bestE;
        const double kp = 0.02, ki = 0.0001;
        cf32 prev = interp(y, pos);
        pos += sps;
        while (pos < (double)i1) {
            const cf32 cur = interp(y, pos), mid = interp(y, pos - sps / 2);
            pw = 0.99 * pw + 0.01 * std::norm(cur);
            const double err = std::real(std::conj(mid) * (cur - prev)) / std::max(pw, 1e-9);
            integ += ki * err;
            pos += sps - kp * err - integ;
            sym.push_back(cur); prev = cur;
        }
        const size_t ns = sym.size(); if (ns < 400) return out_;
        // phase: start from the M-th power average, then a decision-directed loop
        cf32 acc(0, 0); for (size_t k = ns / 8; k < ns / 8 + 300 && k < ns; k++) { const float a = std::abs(sym[k]); if (a > 1e-9f) acc += cpow(sym[k] / a, M); }
        double th = std::arg(acc) / M, fr = 0;
        double amp = 0; for (auto& s : sym) amp += std::abs(s); amp /= (double)ns; if (amp <= 0) return out_;
        const double mu1 = 0.05, mu2 = 0.0008;
        double sIdeal = 0, sErr = 0, sCross = 0; size_t used = 0;
        std::vector<cf32> z(ns), d(ns);
        for (size_t k = 0; k < ns; k++) {
            const cf32 v = sym[k] / (float)amp * cf32((float)std::cos(-th), (float)std::sin(-th));
            const double ph = std::arg(v), kq = std::round(ph * M / (2 * kPi)), id = kq * 2 * kPi / M;
            const cf32 ideal((float)std::cos(id), (float)std::sin(id));
            const double e = std::arg(v * std::conj(ideal));
            fr += mu2 * e; th += mu1 * e + fr;
            z[k] = v; d[k] = ideal;
        }
        for (size_t k = ns / 4; k < ns; k++) { sIdeal += std::norm(d[k]); sCross += std::real(z[k] * std::conj(d[k])); used++; }
        const double g = sCross / std::max(sIdeal, 1e-12);
        for (size_t k = ns / 4; k < ns; k++) sErr += std::norm(z[k] - (float)g * d[k]);
        if (used < 200 || sErr <= 0) return out_;
        out_.ok = true; out_.mer = 10 * std::log10(g * g * sIdeal / sErr); out_.evm = 100 * std::sqrt(sErr / (g * g * sIdeal)); out_.nSym = (int)used;
        return out_;
    }

    Decimator dec_; double rate_ = 1, t_ = 0, lastLogT_ = -1e9, lastMer_ = -99; size_t blockN_ = 0, nf_ = 1, skip_ = 0; std::vector<cf32> buf_;
    bool have_ = false, locked_ = false; int lastM_ = 0, nNo_ = 0; Meas meas_;
};
}

std::unique_ptr<Decoder> makeEvm() { return std::make_unique<EvmDec>(); }

} // namespace dect2::x40
