// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Clean-signal probe of the sens harness (docs/SENSITIVITY-4.0-C1.md). For every mode of tools/sens: is the generator's "clean" signal clean?
// A signal with noise baked in makes every sweep SNR wrong (the signal power the sweep refers to includes that noise: aero, NAVTEX were 5 to 9 dB off).
// Usage: probe_clean [modeRegex]. Per mode: snrdep = power of (clean at snrDb 12 minus clean at 120) against the signal, about -290 dB when the generator
// ignores SynthConfig::snrDb; floor-SNR-in-bw = signal power over the 10th-percentile spectral bin scaled to the noise bandwidth (only meaningful for
// narrow signals in a wide capture); gap(5%) = the 5th percentile of 512-sample windows against the mean: -280 dB or so means exact silence between
// bursts (no noise baked in), 0 dB for a continuous signal. A narrow mode with a floor under about 60 dB or a gap near -10 dB has noise of its own.
#include "sens_modes_d.h"
#include <algorithm>
#include <complex>
#include <cstdio>
#include <regex>
using namespace sens;
static void fft(std::vector<std::complex<double>>& a) {
    size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
    for (size_t len = 2; len <= n; len <<= 1) { double ang = -2 * M_PI / len; std::complex<double> wl(cos(ang), sin(ang));
        for (size_t i = 0; i < n; i += len) { std::complex<double> w(1); for (size_t j = 0; j < len / 2; j++) { auto u = a[i + j], v = a[i + j + len / 2] * w; a[i + j] = u + v; a[i + j + len / 2] = u - v; w *= wl; } } }
}
int main(int argc, char** argv) {
    std::regex re(argc > 1 ? argv[1] : ".*");
    auto all = allModes(); for (auto& m : extraModesD()) all.push_back(std::move(m));
    for (auto& m : all) {
        if (!std::regex_search(m.id, re)) continue;
        if (m.heavy) { printf("%-14s heavy skipped\n", m.id.c_str()); continue; }
        size_t n = (size_t)(std::min(m.secs, 3.0) * m.rate);
        g_cleanSnrDb = 120; Sig x = m.gen(n);
        g_cleanSnrDb = 12; Sig y = m.gen(n); g_cleanSnrDb = 120;
        double dp = 0; for (size_t i = 0; i < x.size() && i < y.size(); i++) dp += std::norm(y[i] - x[i]); dp /= x.size();
        double tot = meanPower(x.data(), x.size());
        if (tot <= 0) { printf("%-14s empty\n", m.id.c_str()); continue; }
        const size_t N = 4096; std::vector<double> psd(N, 0); int cnt = 0;
        for (size_t o = 0; o + N <= x.size() && cnt < 400; o += N / 2, cnt++) {
            std::vector<std::complex<double>> a(N);
            for (size_t i = 0; i < N; i++) { double w = 0.5 - 0.5 * cos(2 * M_PI * i / N); a[i] = std::complex<double>(x[o + i].real(), x[o + i].imag()) * w; }
            fft(a); for (size_t i = 0; i < N; i++) psd[i] += std::norm(a[i]);
        }
        if (!cnt) continue;
        double wsum = N * 0.375; for (auto& p : psd) p /= (cnt * wsum * N);   // power per bin (sum over bins = total)
        std::vector<double> s = psd; std::sort(s.begin(), s.end());
        double floorBin = s[N / 10];                        // 10th percentile bin
        double noiseInBw = floorBin * std::min<double>(N, N * m.bwHz / m.rate);
        double snr = 10 * log10(tot / std::max(noiseInBw, 1e-30));
        // gap power: 5th percentile of 512-sample window power
        std::vector<double> wp; for (size_t o = 0; o + 512 <= x.size(); o += 512) wp.push_back(meanPower(x.data() + o, 512));
        std::sort(wp.begin(), wp.end());
        double gap = 10 * log10(std::max(wp[wp.size() / 20], 1e-30) / tot);
        printf("%-14s snrdep %7.1f dB  rate %9.0f bw %9.0f  floor-SNR-in-bw %6.1f dB  gap(5%%) %6.1f dB  bwfrac %.3f\n", m.id.c_str(), 10*log10(std::max(dp,1e-30)/tot), m.rate, m.bwHz, snr, gap, m.bwHz / m.rate);
    }
}
