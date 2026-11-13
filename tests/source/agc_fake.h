// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A simulated front end for the AGC tests: a radio with a gain model, a wanted signal, thermal noise, strong neighbours (blockers), a DC spike,
// an 8-bit (or deeper) ADC that clips, and a decoder that locks when the in-channel SNR is high enough. Levels are in dB against ADC full scale
// at 0 dB of radio gain, so the level at the ADC is "input level + total gain". Not a physical model of one radio: it has the properties the AGC
// relies on (1 dB of level per dB of gain, noise figure falling with LNA gain, blockers that desensitise the LNA, clipping, quantisation noise).
#pragma once
#include "dect2/source/agc.h"
#include "dect2/sweep/station_health.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace agcsim {

struct Blocker { double offsetHz; double powerDb; };

struct FrontEnd {
    dect2::GainModel model;
    // the radio's present setting
    dect2::GainSetting st;     // HackRF stages
    int total = 0;             // any other radio: the overall gain
    // the scene (input-referred, dB re full scale at 0 dB gain; powers over the channel / the whole rate)
    double sigDb = -60, noiseDb = -80, dcDbfs = -70;
    double rateHz = 10e6, chanBw = 8e6, chanOffset = 0;
    int bits = 8;
    std::vector<Blocker> blockers;
    double lockAt = 8;         // the decoder locks above this SNR
    double kneeDb = 1e9;       // above this gain the decoder gets worse 1 dB per dB without any change in the levels (a distortion the spectrum does not show)
    double jitterDb = 0.3;     // measurement scatter
    uint32_t seed = 12345;

    int gain() const {
        if (model.kind == dect2::GainModel::HackRf) return st.lna + st.vga + (st.amp ? 14 : 0);
        return total;
    }
    void setTotal(int t, bool amp = false, int lnaCap = 40) {
        if (model.kind == dect2::GainModel::HackRf) st = model.split(t, amp, lnaCap);
        else total = t;
    }
    double r01() { seed = seed * 1664525u + 1013904223u; return (double)(seed >> 8) / 16777216.0; }
    double jit() { return (r01() - 0.5) * 2.0 * jitterDb; }
    static double lin(double db) { return std::pow(10.0, db / 10.0); }
    static double dbv(double x) { return 10.0 * std::log10(std::max(x, 1e-30)); }

    double lnaGain() const { return model.kind == dect2::GainModel::HackRf ? st.lna + (st.amp ? 14 : 0) : std::min(gain(), 40); }
    // the noise the front end adds, referred to its input: worse when the first stage has little gain
    double noiseInLin() const { return lin(noiseDb) * (1.0 + lin(22.0 - lnaGain()) * 0.5); }
    // a blocker large at the LNA output desensitises: the in-channel SNR loses what the blocker exceeds the LNA's linear range by
    double lnaOutBlockerDb() const {
        double mx = -300;
        for (const auto& b : blockers) mx = std::max(mx, b.powerDb + lnaGain());
        return mx;
    }
    double desenseDb() const { return std::max(0.0, lnaOutBlockerDb() - (-30.0)); }

    double outPowerDb() const {   // all that reaches the ADC, dBFS rms, including its quantisation noise
        double p = lin(sigDb) + noiseInLin();
        for (const auto& b : blockers) p += lin(b.powerDb);
        p *= lin(gain());
        p += lin(dcDbfs) + lin(dect2::sweep::adcQuantFloorDbfs(bits));
        return dbv(p);
    }
    dect2::SignalStats stats() {
        dect2::SignalStats s;
        const double p = lin(outPowerDb());
        const double sigma = std::sqrt(p / 2.0);
        const double clipC = std::erfc(1.0 / (sigma * std::sqrt(2.0)));   // one component beyond full scale
        s.clipFraction = (float)std::min(1.0, 2 * clipC);
        s.peak = (float)std::min(1.0, 4.0 * sigma);
        if (s.clipFraction > 1e-3) s.peak = 1.0f;
        s.rmsDbfs = (float)std::min(0.0, outPowerDb() + jit());   // a clipped ADC cannot show more than full scale
        return s;
    }
    std::vector<float> spectrum(int n = 1024) {
        std::vector<float> v((size_t)n);
        const double g = lin(gain());
        const double noiseTot = (noiseInLin() * g) + lin(dect2::sweep::adcQuantFloorDbfs(bits));
        const double binHz = rateHz / n;
        const int chBins = std::max(1, (int)std::lround(chanBw / binHz));
        const int c0 = n / 2 + (int)std::lround(chanOffset / binHz);
        for (int i = 0; i < n; i++) {
            double p = noiseTot * 1.5 / n;
            if (std::abs(i - c0) < chBins / 2) p += lin(sigDb) * g * 1.5 / chBins;
            v[(size_t)i] = (float)(dbv(p) + jit());
        }
        for (const auto& b : blockers) {
            const int i = n / 2 + (int)std::lround(b.offsetHz / binHz);
            // a tone leaks over three bins with a Hann window (1, 1/4, 1/4 in power: the bins sum to 1.5 times its power)
            for (int k = -1; k <= 1; k++) if (i + k >= 0 && i + k < n) v[(size_t)(i + k)] = (float)dbv(lin(v[(size_t)(i + k)]) + lin(b.powerDb) * g * (k == 0 ? 1.0 : 0.25));
        }
        v[(size_t)n / 2] = (float)dbv(lin(v[(size_t)n / 2]) + lin(dcDbfs));
        return v;
    }
    // the decoder: locks above 8 dB SNR when the ADC is not clipping hard
    double snrDb() const {
        const double nin = noiseInLin();
        const double q = lin(dect2::sweep::adcQuantFloorDbfs(bits)) / lin(gain());
        return dbv(lin(sigDb) / ((nin + q) * (chanBw / rateHz))) - desenseDb() - std::max(0.0, gain() - kneeDb);
    }
    dect2::AgcQuality quality() {
        dect2::AgcQuality q;
        dect2::SignalStats s = stats();
        const double snr = snrDb();
        q.locked = snr > lockAt && s.clipFraction < 0.01f;
        q.snrValid = q.locked;
        q.snrDb = snr;
        return q;
    }
    dect2::AgcMeasure measure() {
        const dect2::SignalStats s = stats();
        return dect2::measureFront(s, spectrum(), rateHz, chanBw, chanOffset, bits);
    }
};

} // namespace agcsim
