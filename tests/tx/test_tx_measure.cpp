// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TX studio measurements: occupied bandwidth (99 % and -26 dB), PAPR, ACPR, mask check, impairments. Pure functions on synthetic signals with known answers.
// NOTHING is transmitted and no radio is opened.
#include "dect2/tx/tx_measure.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;
using namespace dect2::txm;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const double kPi = 3.14159265358979323846;
static const double kRate = 1e6;
static const int kNfft = 4096;
static const double kBin = kRate / kNfft;

// equal-amplitude tones on every `spacing`-th bin in [k0, k1] (bin offsets from DC), random phases, total mean power `power`
static void tones(std::vector<cf32>& x, int k0, int k1, int spacing, double power, uint32_t seed, size_t n) {
    std::mt19937 g(seed);
    std::vector<int> ks;
    for (int k = k0; k <= k1; k += spacing) ks.push_back(k);
    const double a = std::sqrt(power / (double)ks.size());
    if (x.size() < n) x.resize(n, cf32(0, 0));
    for (int k : ks) {
        const double ph = 2 * kPi * (g() / 4294967296.0), w = 2 * kPi * k * kBin / kRate;
        for (size_t i = 0; i < n; i++) x[i] += cf32((float)(a * std::cos(w * (double)i + ph)), (float)(a * std::sin(w * (double)i + ph)));
    }
}

static double meanPower(const std::vector<cf32>& x) {
    double s = 0;
    for (auto& v : x) s += std::norm(v);
    return s / (double)x.size();
}

int main() {
    const size_t N = 1 << 17;
    // a flat band of 4-bin-spaced tones, bins -100 .. +100: 201 bins wide in 51 tones -> the band is about 200 bins = 48.8 kHz
    std::vector<cf32> band;
    tones(band, -100, 100, 4, 0.25, 1, N);
    const Psd p = welchPsd(band.data(), band.size(), kRate, kNfft, 32);
    CHECK(!p.empty() && p.pw.size() == (size_t)kNfft && std::fabs(p.binHz - kBin) < 1e-9, "psd shape");
    double tot = 0;
    for (double v : p.pw) tot += v;
    CHECK(std::fabs(tot / 0.25 - 1.0) < 0.05, "Parseval: the spectrum sums to the mean power (%.3f of 0.25)", tot);
    CHECK(std::fabs(centroidHz(p)) < 200, "centroid of a symmetric band is at DC (%.1f Hz)", centroidHz(p));

    const double want = 200 * kBin;
    const double o99 = obwHz(p, 0.99), o26 = obwBelowPeakHz(p, 26.0);
    CHECK(std::fabs(o99 - want) / want < 0.06, "OBW 99%% %.0f Hz vs %.0f", o99, want);
    CHECK(std::fabs(o26 - want) / want < 0.12, "OBW -26 dB %.0f Hz vs %.0f", o26, want);
    CHECK(obwHz(p, 0.5) < o99, "a smaller share is a narrower band");
    CHECK(obwHz(Psd(), 0.99) == 0 && obwBelowPeakHz(Psd(), 26) == 0, "empty spectrum gives 0");

    // a shifted band moves the centroid and not the width
    std::vector<cf32> sh;
    tones(sh, 400, 600, 4, 0.25, 2, N);
    const Psd ps = welchPsd(sh.data(), sh.size(), kRate, kNfft, 32);
    CHECK(std::fabs(centroidHz(ps) - 500 * kBin) < 3 * kBin, "centroid of the shifted band %.0f vs %.0f", centroidHz(ps), 500 * kBin);
    CHECK(std::fabs(obwHz(ps, 0.99) - want) / want < 0.06, "shifted band keeps its width");

    // PAPR: a single tone is constant envelope; two equal tones are 3.01 dB; noise is high
    std::vector<cf32> one, two;
    tones(one, 50, 50, 1, 0.5, 3, 20000);
    const Papr p1 = papr(one.data(), one.size());
    CHECK(std::fabs(p1.paprDb) < 0.05, "tone PAPR %.3f dB", p1.paprDb);
    CHECK(std::fabs(p1.rmsDb - 10 * std::log10(0.5)) < 0.05, "tone RMS %.3f dB", p1.rmsDb);
    tones(two, 50, 150, 100, 0.5, 4, 40960);   // two tones, a whole number of beats
    const Papr p2 = papr(two.data(), two.size());
    CHECK(p2.paprDb > 2.7 && p2.paprDb < 3.2, "two-tone PAPR %.3f dB (3.01 expected)", p2.paprDb);
    std::mt19937 g(5);
    std::normal_distribution<float> nd(0, 1);
    std::vector<cf32> nz(200000);
    for (auto& v : nz) v = cf32(nd(g), nd(g));
    const double pn = paprAtProbabilityDb(nz.data(), nz.size(), 0.001);
    CHECK(pn > 7.9 && pn < 9.0, "noise PAPR at 0.1%% %.2f dB (8.4 expected)", pn);
    CHECK(papr(nz.data(), 1).paprDb == 0 && papr(nullptr, 0).paprDb == 0, "too short gives 0");
    std::vector<cf32> zero(100, cf32(0, 0));
    CHECK(papr(zero.data(), zero.size()).paprDb == 0, "silence gives 0");

    // ACPR: a second band 20 dB down next to the first -> upper ACPR about -20 dB, lower clean
    std::vector<cf32> two2;
    tones(two2, -100, 100, 4, 0.25, 6, N);
    tones(two2, 202, 402, 4, 0.0025, 7, N);
    const Psd pa = welchPsd(two2.data(), two2.size(), kRate, kNfft, 32);
    const Acpr a = acpr(pa, o99, 202 * kBin + 100 * kBin, 0);   // the neighbour channel is centred at +302 bins
    CHECK(a.upperValid && a.lowerValid, "both channels fit the span");
    CHECK(std::fabs(a.upperDb + 20.0) < 2.0, "upper ACPR %.1f dB (-20 expected)", a.upperDb);
    CHECK(a.lowerDb < -40, "lower ACPR %.1f dB (clean side)", a.lowerDb);
    const Acpr far = acpr(pa, o99, 0.49 * kRate, 0);   // out of the sampled span
    CHECK(!far.upperValid && !far.lowerValid, "a channel outside the span is not valid");

    // mask: clean band passes, a strong spur at 1.2 x the width fails, a very weak one passes
    const Mask m = genericMask(want);
    CHECK(!m.regulatory, "the generic mask says it is not a standard's");
    CHECK(std::fabs(m.limitAt(0.3 * want)) < 1e-9 && std::fabs(m.limitAt(0.75 * want) + 30) < 1e-9 && std::fabs(m.limitAt(-3 * want) + 50) < 1e-9, "mask points");
    const double mid = m.limitAt(0.625 * want);
    CHECK(mid < 0 && mid > -30, "mask interpolates (%.1f)", mid);
    const MaskResult mr = checkMask(p, m, want, 0);
    CHECK(mr.valid && mr.pass, "clean band passes the mask (excess %.1f dB)", mr.worstExcessDb);
    std::vector<cf32> sp = band;
    tones(sp, 240, 240, 1, 0.25 * 0.01, 8, N);   // spur at 1.2 x the width, 20 dB below the total power
    const MaskResult bad = checkMask(welchPsd(sp.data(), sp.size(), kRate, kNfft, 32), m, want, 0);
    CHECK(bad.valid && !bad.pass && bad.worstExcessDb > 5 && std::fabs(std::fabs(bad.atHz) - 240 * kBin) < 3 * kBin, "strong spur fails (excess %.1f at %.0f Hz)", bad.worstExcessDb, bad.atHz);
    std::vector<cf32> sp2 = band;
    tones(sp2, 240, 240, 1, 0.25 * 1e-9, 9, N);
    CHECK(checkMask(welchPsd(sp2.data(), sp2.size(), kRate, kNfft, 32), m, want, 0).pass, "a spur 90 dB down passes");
    const MaskResult narrow = checkMask(p, m, 0.9 * kRate, 0);   // the band is the whole span: nothing to judge
    CHECK(!narrow.valid, "no verdict when the span does not reach past the band");

    // impairments
    std::vector<cf32> x = band, y = x;
    Impair im;
    im.snrDb = 10;
    applyImpairments(y, kRate, im);
    double en = 0;
    for (size_t i = 0; i < x.size(); i++) en += std::norm(y[i] - x[i]);
    en /= (double)x.size();
    CHECK(std::fabs(10 * std::log10(meanPower(x) / en) - 10.0) < 0.3, "added noise gives the SNR asked (%.2f dB)", 10 * std::log10(meanPower(x) / en));
    std::vector<cf32> y2 = x;
    applyImpairments(y2, kRate, im);
    bool same = true;
    for (size_t i = 0; i < x.size() && same; i++) same = y[i] == y2[i];
    CHECK(same, "same seed gives the same impairment");
    im.seed = 2;
    std::vector<cf32> y3 = x;
    applyImpairments(y3, kRate, im);
    CHECK(y3[100] != y[100], "another seed differs");

    Impair cfo;
    cfo.cfoHz = 5000;
    std::vector<cf32> c = x;
    applyImpairments(c, kRate, cfo);
    const Psd pc = welchPsd(c.data(), c.size(), kRate, kNfft, 32);
    CHECK(std::fabs(centroidHz(pc) - 5000) < 100, "carrier offset moves the centroid by 5 kHz (%.0f)", centroidHz(pc));

    for (int kind = 1; kind <= 2; kind++) {
        Impair f;
        f.fading = kind; f.dopplerHz = 50; f.seed = 3;
        std::vector<cf32> fx = x;
        applyImpairments(fx, kRate, f);
        CHECK(std::fabs(meanPower(fx) / meanPower(x) - 1.0) < 1e-3, "fading %d keeps the mean power", kind);
        double mn = 1e9, mx = 0;
        const size_t blk = 2000;
        for (size_t i = 0; i + blk < fx.size(); i += blk) {
            double s = 0;
            for (size_t k = 0; k < blk; k++) s += std::norm(fx[i + k]);
            mn = std::min(mn, s); mx = std::max(mx, s);
        }
        CHECK(mx / mn > 1.5, "fading %d varies the level (%.2f)", kind, mx / mn);
    }
    Impair none;
    std::vector<cf32> nn = x;
    applyImpairments(nn, kRate, none);
    CHECK(nn == x, "no impairment leaves the signal alone");

    printf(fails ? "tx_measure: %d FAILED\n" : "tx_measure: all passed\n", fails);
    return fails ? 1 : 0;
}
