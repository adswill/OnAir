// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Voice inversion descrambler (voice_inversion.h). Clean-room: a carrier mixer (phase accumulator) and Butterworth / Linkwitz-Riley biquads.
#include "dect2/audio/voice_inversion.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>

namespace dect2 {

const float kVoiceInvPresets[5] = {2550.f, 2700.f, 3000.f, 3300.f, 3400.f};

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Bq {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    float run(float x) { const float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    void set(bool hp, double fc, double rate, double q) {
        const double w = 2 * kPi * std::min(fc, 0.49 * rate) / rate, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
        const double k = hp ? 1 + c : 1 - c;
        b0 = (float)(k / 2 / a0); b1 = (float)((hp ? -k : k) / a0); b2 = b0;
        a1 = (float)(-2 * c / a0); a2 = (float)((1 - al) / a0);
    }
    void clear() { z1 = z2 = 0; }
};
// Two biquads in series: 4th order Butterworth (q 0.5412, 1.3066) or Linkwitz-Riley (0.7071 twice)
struct Casc {
    Bq q[2];
    void set(bool hp, double fc, double rate, bool lr = false) { q[0].set(hp, fc, rate, lr ? 0.70710678 : 0.54119610); q[1].set(hp, fc, rate, lr ? 0.70710678 : 1.30656296); }
    float run(float x) { return q[1].run(q[0].run(x)); }
    void clear() { q[0].clear(); q[1].clear(); }
};

// One mono inverter, either kind
struct Core {
    VoiceInversionSettings s;
    double rate = 48000;
    Casc pre, post, hpIn, lpIn, xoLo, xoHi, postLo, postLo2, postHi;   // postLo + postLo2: 8th order, the low band's image is close (2 * lo above the split)
    double ph1 = 0, ph2 = 0, w1 = 0, w2 = 0;

    void configure(const VoiceInversionSettings& n, bool force = false) {
        const double nyq = 0.45 * rate;
        VoiceInversionSettings c = n;
        c.carrierHz = std::clamp(c.carrierHz, 600.f, (float)nyq);
        c.loHz = std::clamp(c.loHz, 50.f, (float)(nyq - 400));
        c.hiHz = std::clamp(c.hiHz, c.loHz + 400.f, (float)nyq);
        c.splitHz = std::clamp(c.splitHz, c.loHz + 150.f, c.hiHz - 150.f);
        if (!force && c.mode == s.mode && c.carrierHz == s.carrierHz && c.splitHz == s.splitHz && c.loHz == s.loHz && c.hiHz == s.hiHz) return;
        s = c;
        if (s.mode == VoiceInvMode::Simple) {
            pre.set(false, s.carrierHz, rate); post.set(false, s.carrierHz, rate);
            w1 = 2 * kPi * s.carrierHz / rate;
        } else {
            hpIn.set(true, s.loHz, rate); lpIn.set(false, s.hiHz, rate);
            xoLo.set(false, s.splitHz, rate, true); xoHi.set(true, s.splitHz, rate, true);
            postLo.set(false, s.splitHz + 0.25 * s.loHz, rate); postLo2.set(false, s.splitHz + 0.25 * s.loHz, rate); postHi.set(false, 1.1 * s.hiHz, rate);
            w1 = 2 * kPi * (s.loHz + s.splitHz) / rate;   // [lo, split] -> [lo, split] mirrored
            w2 = 2 * kPi * (s.splitHz + s.hiHz) / rate;   // [split, hi] mirrored
        }
    }
    void clear() { pre.clear(); post.clear(); hpIn.clear(); lpIn.clear(); xoLo.clear(); xoHi.clear(); postLo.clear(); postLo2.clear(); postHi.clear(); }
    float tick(float m) {
        float v;
        if (s.mode == VoiceInvMode::Simple) {
            v = post.run(2.f * pre.run(m) * (float)std::cos(ph1));
            ph1 += w1; if (ph1 > 2 * kPi) ph1 -= 2 * kPi;
        } else {
            const float b = lpIn.run(hpIn.run(m));
            const float lo = postLo2.run(postLo.run(2.f * xoLo.run(b) * (float)std::cos(ph1)));
            const float hi = postHi.run(2.f * xoHi.run(b) * (float)std::cos(ph2));
            ph1 += w1; if (ph1 > 2 * kPi) ph1 -= 2 * kPi;
            ph2 += w2; if (ph2 > 2 * kPi) ph2 -= 2 * kPi;
            v = lo + hi;
        }
        return v;
    }
};

}  // namespace

struct VoiceInversion::Impl {
    Core core;
    double wet = 0, step = 0.01;
    int rate = 48000;
    bool wasOff = true;
};

VoiceInversionConfig::VoiceInversionConfig() : cap(new std::atomic<float>[kCapN]) { for (int i = 0; i < kCapN; i++) cap[i].store(0.f, std::memory_order_relaxed); }
VoiceInversionConfig& VoiceInversionConfig::global() { static VoiceInversionConfig c; return c; }
void VoiceInversionConfig::set(const VoiceInversionSettings& s) {
    mode.store((int)s.mode); carrierHz.store(s.carrierHz); splitHz.store(s.splitHz); loHz.store(s.loHz); hiHz.store(s.hiHz); on.store(s.on);
}
VoiceInversionSettings VoiceInversionConfig::get() const {
    VoiceInversionSettings s;
    s.on = on.load(); s.mode = mode.load() == 1 ? VoiceInvMode::Split : VoiceInvMode::Simple;
    s.carrierHz = carrierHz.load(); s.splitHz = splitHz.load(); s.loHz = loHz.load(); s.hiHz = hiHz.load();
    return s;
}
int VoiceInversionConfig::snapshot(std::vector<float>& out, double seconds) const {
    const int r = capRate.load();
    const unsigned long long w = capW.load(std::memory_order_acquire);
    long long n = (long long)(seconds * r);
    n = std::min<long long>({n, kCapN - 4096, (long long)w});
    out.resize((size_t)std::max<long long>(n, 0));
    for (long long i = 0; i < n; i++) out[(size_t)i] = cap[(size_t)((w - n + i) % kCapN)].load(std::memory_order_relaxed);
    return n > 0 ? r : 0;
}

VoiceInversion::VoiceInversion() : p_(new Impl), cfg_(&VoiceInversionConfig::global()) {}
VoiceInversion::~VoiceInversion() = default;

void VoiceInversion::prepare(int sampleRate) {
    p_->rate = std::max(sampleRate, 8000);
    p_->core.rate = p_->rate;
    p_->step = 1.0 / (0.01 * p_->rate);
    p_->core.configure(cfg_->get(), true);
    reset();
}
void VoiceInversion::reset() { p_->core.clear(); p_->core.ph1 = p_->core.ph2 = 0; p_->wet = 0; p_->wasOff = true; }

const float* VoiceInversion::process(const float* in, int frames, float* scratch, bool* active) {
    if (active) *active = false;
    const bool on = cfg_->on.load(std::memory_order_relaxed);
    const bool cap = on || cfg_->capture.load(std::memory_order_relaxed);
    if (!on && p_->wet <= 0.0 && !cap) return in;
    frames = std::min(frames, (int)kMaxChunk);
    if (cap) {   // the sound going in, mono
        VoiceInversionConfig& c = *cfg_;
        c.capRate.store(p_->rate, std::memory_order_relaxed);
        unsigned long long w = c.capW.load(std::memory_order_relaxed);
        for (int i = 0; i < frames; i++, w++) c.cap[(size_t)(w % VoiceInversionConfig::kCapN)].store(0.5f * (in[2 * i] + in[2 * i + 1]), std::memory_order_relaxed);
        c.capW.store(w, std::memory_order_release);
    }
    if (!on && p_->wet <= 0.0) return in;
    Impl& d = *p_;
    if (on) {
        if (d.wasOff) { d.core.clear(); d.core.ph1 = d.core.ph2 = 0; d.wasOff = false; }
        d.core.configure(cfg_->get());
    } else d.wasOff = true;
    const double target = on ? 1.0 : 0.0;
    for (int i = 0; i < frames; i++) {
        const float m = 0.5f * (in[2 * i] + in[2 * i + 1]);
        const float v = d.core.tick(m);
        if (d.wet < target) d.wet = std::min(target, d.wet + d.step); else if (d.wet > target) d.wet = std::max(target, d.wet - d.step);
        const float o = (float)(d.wet * v + (1.0 - d.wet) * m);
        scratch[2 * i] = scratch[2 * i + 1] = o;
    }
    if (active) *active = true;
    return scratch;
}

// ---- scoring and search

double voiceSpeechScore(const float* x, int n, int rate, double* modulation) {
    if (modulation) *modulation = 0;
    constexpr int N = 1024;
    if (n < 2 * N || rate < 8000) return 0.0;
    static thread_local Fft fft(N);
    std::vector<std::complex<float>> buf(N);
    std::vector<double> psd(N / 2 + 1, 0.0), win(N);
    for (int i = 0; i < N; i++) win[(size_t)i] = 0.5 - 0.5 * std::cos(2 * kPi * i / N);
    for (int s = 0; s + N <= n; s += N / 2) {
        for (int i = 0; i < N; i++) buf[(size_t)i] = std::complex<float>((float)(x[s + i] * win[(size_t)i]), 0.f);
        fft.forward(buf.data());
        for (int k = 0; k <= N / 2; k++) psd[(size_t)k] += std::norm(buf[(size_t)k]);
    }
    auto E = [&](double a, double b) {
        double e = 0;
        const int k0 = std::max(0, (int)std::ceil(a * N / rate)), k1 = std::min(N / 2, (int)std::floor(b * N / rate));
        for (int k = k0; k <= k1; k++) e += psd[(size_t)k];
        return e;
    };
    const double total = E(0, rate / 2.0) + 1e-20;
    const double inband = E(250, 3400) / total, sub = E(0, 120) / total, over = E(3600, rate / 2.0) / total;
    const double f1 = std::min(1.0, E(250, 900) / total / 0.45);   // the voice's lowest harmonics and first formant: where a correct result has its energy
    const double lowE = E(250, 1200), highE = E(2200, 3400);
    const double tiltDb = 10 * std::log10((lowE + 1e-20) / (highE + 1e-20));
    const double tilt = std::clamp(tiltDb / 8.0, 0.0, 1.0);   // saturates: a bit more tilt must not win over the real thing
    // syllable modulation: the envelope on 100 Hz frames, its power at 2..8 Hz against everything but DC
    const int hop = rate / 100, nf = n / hop;
    double mod = 0;
    if (nf >= 128) {
        std::vector<double> env((size_t)nf);
        double mean = 0;
        for (int f = 0; f < nf; f++) { double e = 0; for (int i = 0; i < hop; i++) e += (double)x[f * hop + i] * x[f * hop + i]; env[(size_t)f] = std::sqrt(e / hop); mean += env[(size_t)f]; }
        mean /= nf;
        double tot = 0, band = 0;
        const int M = std::min(nf, 512);
        for (int k = 1; k <= M / 2; k++) {
            double re = 0, im = 0;
            for (int f = 0; f < M; f++) { const double wv = 0.5 - 0.5 * std::cos(2 * kPi * f / M), v = (env[(size_t)f] - mean) * wv; re += v * std::cos(2 * kPi * k * f / M); im -= v * std::sin(2 * kPi * k * f / M); }
            const double p = re * re + im * im, hz = 100.0 * k / M;
            tot += p; if (hz >= 2 && hz <= 8) band += p;
        }
        mod = tot > 0 ? band / tot : 0;
    }
    if (modulation) *modulation = mod;
    return inband * f1 * (0.25 + 0.75 * tilt) * (1 - std::min(1.0, 4 * sub)) * (1 - over);
}

namespace {
double scoreCandidate(const float* x, int n, int rate, const VoiceInversionSettings& s) {
    Core c; c.rate = rate; c.configure(s, true);
    std::vector<float> y((size_t)n);
    for (int i = 0; i < n; i++) y[(size_t)i] = c.tick(x[i]);
    const int skip = rate / 10;
    return voiceSpeechScore(y.data() + skip, n - skip, rate);
}
}  // namespace

VoiceInvSearchResult voiceInversionSearch(const float* x, int n, int rate, VoiceInvMode mode, const VoiceInversionSettings& base,
                                          std::atomic<float>* progress, const std::atomic<bool>* cancel) {
    VoiceInvSearchResult r;
    if (progress) progress->store(0.f);
    if (n < rate || rate < 8000) return r;
    n = std::min(n, 6 * rate);
    double mod = 0;
    r.baseline = voiceSpeechScore(x, n, rate, &mod);
    r.modulation = mod;
    double lo, hi, step;
    VoiceInversionSettings s = base; s.mode = mode;
    if (mode == VoiceInvMode::Simple) { lo = 2000; hi = std::min(3600.0, 0.45 * rate); step = 50; }
    else { lo = base.loHz + 250; hi = base.hiHz - 250; step = 50; }
    if (hi <= lo) return r;
    auto at = [&](double v) { VoiceInversionSettings t = s; if (mode == VoiceInvMode::Simple) t.carrierHz = (float)v; else t.splitHz = (float)v; return scoreCandidate(x, n, rate, t); };
    const int total = (int)((hi - lo) / step) + 1;
    std::vector<double> sc;
    for (int i = 0; i < total; i++) {
        if (cancel && cancel->load()) return r;
        sc.push_back(at(lo + i * step));
        if (progress) progress->store((float)(i + 1) / total);
    }
    int bi = (int)(std::max_element(sc.begin(), sc.end()) - sc.begin());
    const double best = sc[(size_t)bi];
    // the score has a plateau around the right value (a speech-like result stays speech-like a little off): take the middle of it
    int a0 = bi, a1 = bi;
    while (a0 > 0 && sc[(size_t)a0 - 1] >= 0.92 * best) a0--;
    while (a1 + 1 < total && sc[(size_t)a1 + 1] >= 0.92 * best) a1++;
    r.value = (float)(lo + 0.5 * (a0 + a1) * step); r.score = best;
    r.found = mod >= 0.25 && best > 0.35 && best > 1.15 * r.baseline;
    if (progress) progress->store(1.f);
    return r;
}

}  // namespace dect2
