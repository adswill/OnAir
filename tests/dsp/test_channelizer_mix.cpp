// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The carrier channeliser (carrierfind.h) and the rational resampler after the speed work:
//  - RationalResampler gives the same samples however the input is cut into blocks (its buffer keeps a consumed head between calls), to the
//    last bit with the portable kernels and within 1e-6 with Accelerate (vDSP_desamp's own rounding depends on the length of the call);
//  - Channelizer's table mixer (float table plus a double phasor per 1024 samples) against a plain std::complex<double> phasor mixer feeding
//    the same resampler: within 1e-6 at an rms of 0.2 (the float products), over 10^6 samples so phasor drift would show;
//  - CarrierFinder finds the same carriers when it is fed in odd block sizes.
#include "dect2/dsp/carrierfind.h"
#include "dect2/dsp/resampler.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

using namespace dect2;

namespace {
int fails = 0;
bool close(const std::vector<cf32>& a, const std::vector<cf32>& b) {
    if (a.size() != b.size()) return false;
#if defined(__APPLE__) && !defined(DECT2_PORTABLE)
    const float tol = 1e-6f;
#else
    const float tol = 0.f;
#endif
    for (size_t i = 0; i < a.size(); i++) if (std::abs(a[i] - b[i]) > tol) return false;
    return true;
}
void check(bool ok, const char* what) { printf("%s: %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

std::vector<cf32> tones(size_t n, double rate) {
    std::vector<cf32> v(n);
    std::mt19937 rng(5);
    std::normal_distribution<float> nd(0.f, 0.02f);
    const double f[3] = {-412500, 12500, 300000};
    for (size_t i = 0; i < n; i++) {
        std::complex<double> s = 0;
        for (double hz : f) s += std::polar(0.2, 2 * M_PI * hz * (double)i / rate);
        v[i] = cf32((float)s.real() + nd(rng), (float)s.imag() + nd(rng));
    }
    return v;
}
} // namespace

int main() {
    const double rate = 2.4e6;
    const auto x = tones(1000000, rate);
    {   // block-size independence of the resampler
        RationalResampler a, b; a.configure(rate, 48e3, nullptr, 12500); b.configure(rate, 48e3, nullptr, 12500);
        std::vector<cf32> oa, ob;
        a.process(x.data(), x.size(), oa);
        std::mt19937 rng(1);
        for (size_t i = 0; i < x.size();) { const size_t n = std::min<size_t>(x.size() - i, 1 + rng() % 9000); b.process(x.data() + i, n, ob); i += n; }
        check(close(oa, ob), "RationalResampler: same output for one block and for random blocks");
        RationalResampler c, d; c.configure(10e6, 64e6 / 7); d.configure(10e6, 64e6 / 7);
        std::vector<cf32> oc, od;
        c.process(x.data(), x.size(), oc);
        for (size_t i = 0; i < x.size(); i += 16384) d.process(x.data() + i, std::min<size_t>(16384, x.size() - i), od);
        check(close(oc, od), "RationalResampler 10 -> 9.142857 Msps: same output in 16384-sample blocks");
    }
    {   // table mixer against a double phasor
        const double shift = 300000 + 37.5;
        Channelizer ch; ch.configure(rate, 48e3, shift, 12500);
        RationalResampler ref; ref.configure(rate, 48e3, nullptr, 12500);
        std::vector<cf32> o1, o2, mix(x.size());
        const std::complex<double> st = std::polar(1.0, -2 * M_PI * shift / rate);
        std::complex<double> p(1, 0);
        for (size_t i = 0; i < x.size(); i++) { mix[i] = cf32((float)(x[i].real() * p.real() - x[i].imag() * p.imag()), (float)(x[i].real() * p.imag() + x[i].imag() * p.real())); p *= st; }
        ref.process(mix.data(), mix.size(), o2);
        for (size_t i = 0; i < x.size(); i += 5000) ch.process(x.data() + i, std::min<size_t>(5000, x.size() - i), o1);
        double err = 0, pw = 0;
        const size_t n = std::min(o1.size(), o2.size());
        for (size_t i = 0; i < n; i++) { err = std::max(err, (double)std::abs(o1[i] - o2[i])); pw += std::norm(o2[i]); }
        const double rms = std::sqrt(pw / std::max<size_t>(1, n));
        printf("      table mixer: max error %.2e of rms %.3f over %zu outputs\n", err, rms, n);
        check(o1.size() == o2.size() && rms > 0.1 && err < 1e-6, "Channelizer table mixer matches a double phasor mixer");
    }
    {   // the finder in odd block sizes
        CarrierCfg cfg; cfg.stepHz = 12500; cfg.debounceSec = 0.05;
        CarrierFinder a, b; a.configure(rate, cfg); b.configure(rate, cfg);
        a.feed(x.data(), x.size());
        for (size_t i = 0; i < x.size(); i += 777) b.feed(x.data() + i, std::min<size_t>(777, x.size() - i));
        bool same = a.carriers().size() == b.carriers().size() && a.carriers().size() == 3;
        for (size_t i = 0; same && i < a.carriers().size(); i++) same = a.carriers()[i].hz == b.carriers()[i].hz && a.carriers()[i].levelDb == b.carriers()[i].levelDb;
        printf("      finder: %zu carriers\n", a.carriers().size());
        check(same, "CarrierFinder: the three carriers, the same for one block and for 777-sample blocks");
    }
    printf(fails ? "channelizer mix: %d failure(s)\n" : "channelizer mix: all passed\n", fails);
    return fails ? 1 : 0;
}
