// AM clarity harness: objective checks that the AM processor does not sound "robotic" (musical noise, pumping, bandwidth flapping,
// distortion of a clean signal) and that it still improves the speech-band SNR of a weak signal.
// Run with an argument ("print") to list the numbers only.
#include "dect2/general/am_proc.h"
#include "dect2/dsp/fftutil.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using namespace dect2;
static const double kPi = 3.14159265358979323846, FS = 48000;

// speech-like audio: pitch pulses (110..160 Hz) through three formants, syllable envelope about 4 Hz with pauses; peak 1
static std::vector<float> speechLike(double sec, unsigned seed) {
    std::mt19937 g(seed); std::uniform_real_distribution<double> u(0, 1);
    const size_t n = (size_t)(sec * FS); std::vector<float> a(n);
    double ph = 0, f0 = 120, env = 0, envT = 0; double f1[3] = {600, 1200, 2600}, bw[3] = {90, 110, 160};
    double y1[3] = {0, 0, 0}, y2[3] = {0, 0, 0}; size_t nextSyl = 0; bool voiced = true;
    for (size_t i = 0; i < n; i++) {
        if (i >= nextSyl) { nextSyl = i + (size_t)(FS * (0.12 + 0.18 * u(g))); voiced = u(g) > 0.2; envT = voiced ? 0.4 + 0.6 * u(g) : 0;
            f0 = 100 + 60 * u(g); for (int k = 0; k < 3; k++) f1[k] = (k == 0 ? 400 + 400 * u(g) : k == 1 ? 900 + 1200 * u(g) : 2200 + 800 * u(g)); }
        env += (envT - env) * 0.002;
        ph += f0 / FS; double x = 0; if (ph >= 1) { ph -= 1; x = 1; }
        double s = 0; for (int k = 0; k < 3; k++) { const double r = std::exp(-kPi * bw[k] / FS), w = 2 * kPi * f1[k] / FS;
            const double y = x + 2 * r * std::cos(w) * y1[k] - r * r * y2[k]; y2[k] = y1[k]; y1[k] = y; s += y * (k == 0 ? 1.0 : 0.6 / (k + 1)); }
        a[i] = (float)(s * env * (1 + 0.0 * u(g)));
    }
    for (int pass = 0; pass < 3; pass++) { double z = 0; const double k = 1 - std::exp(-2 * kPi * 2000 / FS); for (auto& v : a) { z += k * (v - z); v = (float)z; } }   // inside the AM audio band
    double pk = 1e-9; for (float v : a) pk = std::max(pk, (double)std::fabs(v)); for (auto& v : a) v = (float)(v / pk); return a;
}
static std::vector<float> musicLike(double sec) {
    const size_t n = (size_t)(sec * FS); std::vector<float> a(n, 0.f);
    const double notes[6] = {262, 330, 392, 523, 659, 784};
    for (size_t i = 0; i < n; i++) { const double t = i / FS; const int nn = (int)(t * 3) % 6; const double lt = t * 3 - std::floor(t * 3);
        const double e = std::exp(-3 * lt); double s = 0; for (int h = 1; h <= 5; h++) s += std::sin(2 * kPi * notes[nn] * h * t) / (h * h);
        a[i] = (float)(s * e); }
    double pk = 1e-9; for (float v : a) pk = std::max(pk, (double)std::fabs(v)); for (auto& v : a) v = (float)(v / pk); return a;
}
struct Opt { double cn = 40; bool fade = false, impulse = false; double m = 0.8; };
static std::vector<cf32> makeAm(const std::vector<float>& au, const Opt& o, unsigned seed) {
    std::mt19937 g(seed); std::normal_distribution<double> nd(0, 1); std::uniform_real_distribution<double> u(0, 1);
    const size_t n = au.size(); std::vector<cf32> x(n);
    const double off = 150, sig = std::sqrt(0.5 / std::pow(10.0, o.cn / 10) * 48.0 / 10.0);   // C/N over 10 kHz, carrier power 1
    std::vector<cf32> s(n); for (size_t i = 0; i < n; i++) { const double a = 1 + o.m * au[i]; s[i] = cf32((float)(a * std::cos(2 * kPi * off * i / FS)), (float)(a * std::sin(2 * kPi * off * i / FS))); }
    const int d = (int)(0.0004 * FS);
    for (size_t i = 0; i < n; i++) { cf32 v = s[i];
        if (o.fade && i >= (size_t)d) { const double ph = 2 * kPi * 0.7 * i / FS, k = 0.4 + 0.4 * std::sin(2 * kPi * 0.3 * i / FS); v = cf32((float)(0.6 * v.real()), (float)(0.6 * v.imag())) + (float)k * s[i - d] * cf32((float)std::cos(ph), (float)std::sin(ph)); }
        x[i] = v + cf32((float)(sig * nd(g)), (float)(sig * nd(g))); }
    if (o.impulse) for (size_t i = 2000; i + 4 < n; i += (size_t)(FS / 20 * (0.5 + u(g)))) for (int k = 0; k < 3; k++) x[i + (size_t)k] += cf32((float)(6 * nd(g)), (float)(6 * nd(g)));
    return x;
}
struct Run { std::vector<float> out; std::vector<float> bw; };
static Run runProc(const std::vector<cf32>& x, int nr = -1) {
    AmProcessor p; p.configure(FS); AmProcConfig c; c.nr = nr; p.setConfig(c);
    Run r; const size_t B = 1024;
    for (size_t i = 0; i < x.size(); i += B) { const size_t m = std::min(B, x.size() - i); p.process(x.data() + i, m, r.out); r.bw.push_back(p.status().bandwidthHz); }
    return r;
}
// SNR of out against the reference audio (delay search, best gain per 0.5 s block so the leveler is not counted as distortion)
static double refSnr(const std::vector<float>& ref, const std::vector<float>& out, double from, double to) {
    int bestD = 0; double bestC = -1;
    const size_t a0 = (size_t)(from * FS), a1 = std::min(ref.size(), (size_t)(to * FS));
    for (int d = 0; d <= 4000; d += 1) { double c = 0, e1 = 1e-9, e2 = 1e-9; for (size_t i = a0; i < a1; i += 5) { if (i + (size_t)d >= out.size()) break; c += ref[i] * out[i + (size_t)d]; e1 += ref[i] * ref[i]; e2 += out[i + (size_t)d] * out[i + (size_t)d]; }
        c = std::fabs(c) / std::sqrt(e1 * e2); if (c > bestC) { bestC = c; bestD = d; } }
    double es = 0, en = 0; const size_t blk = (size_t)(0.5 * FS);
    for (size_t b = a0; b + blk <= a1; b += blk) { double sr = 0, so = 0, sx = 0; for (size_t i = b; i < b + blk; i++) { const double o = out[i + (size_t)bestD], r = ref[i]; sr += r * r; so += o * o; sx += o * r; }
        if (sr < 1e-6 || so < 1e-9) continue; const double g = sx / so; en += sr + g * g * so - 2 * g * sx; es += sr; }
    return 10 * std::log10(es / std::max(en, 1e-12));
}
// musical noise: the normalised variance of the per-bin power over time (white Gaussian noise: 1)
static double musicalIndex(const std::vector<float>& y, size_t from) {
    const int N = 512; Fft f(N); std::vector<double> mean(N / 2, 0), m2(N / 2, 0); int fr = 0;
    std::vector<cf32> b(N);
    for (size_t s = from; s + N <= y.size(); s += N / 2) { for (int k = 0; k < N; k++) b[(size_t)k] = cf32(y[s + (size_t)k] * (float)(0.5 - 0.5 * std::cos(2 * kPi * k / N)), 0.f); f.forward(b.data());
        for (int k = 0; k < N / 2; k++) { const double p = std::norm(b[(size_t)k]); mean[(size_t)k] += p; m2[(size_t)k] += p * p; } fr++; }
    double acc = 0; int cnt = 0; for (int k = (int)(300.0 * N / FS); k < (int)(3000.0 * N / FS); k++) { const double m = mean[(size_t)k] / fr, v = m2[(size_t)k] / fr - m * m; acc += v / (m * m + 1e-30); cnt++; }
    return acc / cnt;
}
static int fails = 0;
static void chk(bool ok, const char* what, double v, double lim) { printf("  %-46s %8.3f (limit %.3f) %s\n", what, v, lim, ok ? "ok" : "FAIL"); if (!ok) fails++; }

int main(int argc, char**) {
    const bool printOnly = argc > 1;
    const auto sp = speechLike(12, 7), mu = musicLike(12);
    // (a) musical noise: a carrier with no programme and noise only (C/N 12 dB)
    { std::vector<float> sil(sp.size(), 0.f); Opt o; o.cn = 12; const auto r = runProc(makeAm(sil, o, 1));
      const double mi = musicalIndex(r.out, (size_t)(5 * FS)); chk(printOnly || mi < 1.40, "musical-noise index, noise only (lower better)", mi, 1.40);
      std::vector<double> e; for (size_t s = (size_t)(5 * FS); s + 960 <= r.out.size(); s += 960) { double a = 0; for (size_t i = s; i < s + 960; i++) a += r.out[i] * r.out[i]; e.push_back(10 * std::log10(a / 960 + 1e-12)); }
      double m = 0, v = 0; for (double q : e) m += q; m /= (double)e.size(); for (double q : e) v += (q - m) * (q - m); const double sd = std::sqrt(v / (double)e.size());
      chk(printOnly || sd < 0.95, "residual noise level flutter, 20 ms std dB", sd, 0.95); }
    // (b) speech-band SNR improvement of weak signals against the unprocessed chain
    for (double cn : {12.0, 18.0, 25.0}) { Opt o; o.cn = cn; const auto x = makeAm(sp, o, 2); const auto a = runProc(x), off = runProc(x, 0);
      const double sa = refSnr(sp, a.out, 3, 11), so = refSnr(sp, off.out, 3, 11); char nm[64]; snprintf(nm, sizeof nm, "speech SNR gain vs NR off, C/N %.0f dB", cn);
      chk(printOnly || sa - so > 0.8 + (cn < 20 ? 1.0 : 0.0), nm, sa - so, 0.8 + (cn < 20 ? 1.0 : 0.0)); printf("    (SNR with %.2f dB, without %.2f dB)\n", sa, so); }
    // (c) pumping: a steady 1 kHz tone, level variation of the output
    { std::vector<float> t(sp.size()); for (size_t i = 0; i < t.size(); i++) t[i] = (float)std::sin(2 * kPi * 1000 * i / FS); Opt o; o.cn = 24; const auto r = runProc(makeAm(t, o, 3));
      std::vector<double> e; for (size_t s = (size_t)(4 * FS); s + 2400 <= r.out.size(); s += 2400) { double a = 0; for (size_t i = s; i < s + 2400; i++) a += r.out[i] * r.out[i]; e.push_back(10 * std::log10(a / 2400 + 1e-12)); }
      double m = 0, v = 0; for (double q : e) m += q; m /= (double)e.size(); for (double q : e) v += (q - m) * (q - m); const double sd = std::sqrt(v / (double)e.size());
      chk(printOnly || sd < 0.6, "tone level std dev in dB (pumping)", sd, 0.6); }
    // (d) bandwidth switches per minute, speech C/N 20 with selective fading and impulses
    { Opt o; o.cn = 20; o.fade = true; o.impulse = true; const auto r = runProc(makeAm(sp, o, 4)); int sw = 0; for (size_t i = 60; i < r.bw.size(); i++) if (r.bw[i] != r.bw[i - 1]) sw++;
      const double rate = sw * 60.0 / (12.0 - 60.0 * 1024 / FS); chk(printOnly || rate < 20, "bandwidth changes per minute", rate, 20); }
    // (e) a clean signal: speech and music pass nearly unchanged
    for (int mus = 0; mus < 2; mus++) { Opt o; o.cn = 70; const auto& au = mus ? mu : sp; const auto r = runProc(makeAm(au, o, 5)); const double s = refSnr(au, r.out, 3, 11);
      chk(printOnly || s > (mus ? 20 : 16), mus ? "clean music vs input audio, SNR dB" : "clean speech vs input audio, SNR dB", s, mus ? 20 : 16); }
    // (f) impulses and fading must not wreck speech: SNR against the reference at C/N 30
    { Opt o; o.cn = 30; o.impulse = true; const auto r = runProc(makeAm(sp, o, 6)); const double s = refSnr(sp, r.out, 3, 11); chk(printOnly || s > 14.5, "speech with impulse noise, C/N 30, SNR dB", s, 14.5); }
    printf(fails ? "am_clear: FAIL\n" : "am_clear: ok\n");
    return fails ? 1 : 0;
}
