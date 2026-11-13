// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OnAir Web DSP on synthetic IQ: the shared overlap-save FFT and the per-user slices (a tone comes out where it should with a continuous phase),
// every demodulator (AM, NFM, WFM, USB, LSB, CW) gives the modulating tone, the passband rejects a neighbour, squelch, the waterfall rows.
#include "../../server/web/web_dsp.h"
#include <cmath>
#include <complex>
#include <functional>
#include <cstdio>
#include <vector>
using namespace onair_web;
#include "../common/test_util.h"

// the power of `f` Hz in x relative to the total (a single-bin DFT over the whole buffer, skipping the first `skip` samples)
static double toneFraction(const std::vector<float>& x, double rate, double f, size_t skip) {
    double re = 0, im = 0, tot = 0;
    size_t n = 0;
    for (size_t i = skip; i < x.size(); i++, n++) {
        const double a = 2 * M_PI * f * (double)i / rate;
        re += x[i] * std::cos(a); im -= x[i] * std::sin(a);
        tot += (double)x[i] * x[i];
    }
    if (!n || tot <= 0) return 0;
    return 2.0 * (re * re + im * im) / ((double)n * tot);
}

using Gen = std::function<cf32(double t)>;
static std::vector<float> runRx(double rate, const RxSettings& s, Gen g, double seconds, float* level = nullptr, bool* sq = nullptr) {
    SharedFft f;
    f.configure(rate);
    VirtualRx rx;
    std::string err;
    if (!rx.configure(f, s, err)) { printf("configure: %s\n", err.c_str()); return {}; }
    std::vector<float> audio;
    const size_t total = (size_t)(rate * seconds), chunk = 8192;
    std::vector<cf32> buf(chunk);
    for (size_t i = 0; i < total; i += chunk) {
        for (size_t k = 0; k < chunk; k++) buf[k] = g((double)(i + k) / rate);
        f.push(buf.data(), chunk, [&](const float* re, const float* im) { rx.process(re, im, f.block(), audio); });
    }
    if (level) *level = rx.levelDb();
    if (sq) *sq = rx.squelched();
    return audio;
}

static void testSlicePhase() {
    // a tone exactly at the slice's frequency gives a constant phasor; one 700 Hz off gives a clean 700 Hz rotation
    const double rate = 2.4e6;
    for (double off : {123456.7, -987654.3, 0.0, 333.0}) {
        SharedFft f;
        f.configure(rate);
        SliceIq sl;
        std::string err;
        CHECK(sl.configure(f, off, 48000, err), "slice: %s", err.c_str());
        std::vector<cf32> out;
        std::vector<cf32> buf(10000);
        size_t pos = 0;
        for (int b = 0; b < 60; b++) {
            for (size_t k = 0; k < buf.size(); k++, pos++) { const double a = 2 * M_PI * (off + 700.0) * (double)pos / rate; buf[k] = cf32((float)std::cos(a), (float)std::sin(a)) * 0.5f; }
            f.push(buf.data(), buf.size(), [&](const float* re, const float* im) { sl.process(re, im, f.block(), out); });
        }
        CHECK(out.size() > 10000, "slice output %zu", out.size());
        // phase steps after the start-up: all close to 2 pi 700 / 48000
        const double want = 2 * M_PI * 700.0 / 48000.0;
        double worst = 0, amp = 0;
        size_t n = 0;
        for (size_t i = 2000; i < out.size(); i++, n++) {
            const double d = std::arg(out[i] * std::conj(out[i - 1]));
            worst = std::max(worst, std::fabs(d - want));
            amp += std::abs(out[i]);
        }
        amp /= (double)n;
        CHECK(worst < 0.02, "offset %.1f: worst phase step error %.4f rad", off, worst);
        CHECK(std::fabs(amp - 0.5) < 0.03, "offset %.1f: amplitude %.3f (want 0.5)", off, amp);
    }
}

static void testDemods() {
    const double rate = 2.4e6, fc = 250000;
    const double fm = 1000;   // modulating tone
    struct Case { Demod d; Gen g; double tuneOff; double toneHz; const char* name; };
    std::vector<Case> cs = {
        {Demod::Usb, [&](double t) { const double a = 2 * M_PI * (fc + fm) * t; return cf32((float)std::cos(a), (float)std::sin(a)) * 0.1f; }, fc, fm, "usb"},
        {Demod::Lsb, [&](double t) { const double a = 2 * M_PI * (fc - fm) * t; return cf32((float)std::cos(a), (float)std::sin(a)) * 0.1f; }, fc, fm, "lsb"},
        {Demod::Cw, [&](double t) { const double a = 2 * M_PI * fc * t; return cf32((float)std::cos(a), (float)std::sin(a)) * 0.1f; }, fc, 700, "cw"},
        {Demod::Am, [&](double t) { const double a = 2 * M_PI * fc * t; const float m = (float)(1 + 0.5 * std::sin(2 * M_PI * fm * t)); return cf32((float)std::cos(a), (float)std::sin(a)) * (0.1f * m); }, fc, fm, "am"},
        {Demod::Nfm, [&](double t) { const double ph = 2 * M_PI * fc * t - (3000.0 / fm) * std::cos(2 * M_PI * fm * t); return cf32((float)std::cos(ph), (float)std::sin(ph)) * 0.1f; }, fc, fm, "nfm"},
        {Demod::Wfm, [&](double t) { const double ph = 2 * M_PI * fc * t - (50000.0 / fm) * std::cos(2 * M_PI * fm * t); return cf32((float)std::cos(ph), (float)std::sin(ph)) * 0.1f; }, fc, fm, "wfm"},
    };
    for (auto& c : cs) {
        RxSettings s;
        s.offsetHz = c.tuneOff; s.demod = c.d; s.audioRate = 12000; s.deemphUs = 0;
        const auto a = runRx(rate, s, c.g, 0.6);
        CHECK(a.size() > 5000, "%s: %zu audio samples", c.name, a.size());
        const double frac = toneFraction(a, 12000, c.toneHz, 2400);
        CHECK(frac > 0.8, "%s: tone %.0f Hz holds only %.3f of the audio power", c.name, c.toneHz, frac);
        double pk = 0;
        for (size_t i = 2400; i < a.size(); i++) pk = std::max(pk, (double)std::fabs(a[i]));
        CHECK(pk > 0.05 && pk <= 1.0, "%s: audio peak %.3f", c.name, pk);
    }
}

static void testSelectivityAndSquelch() {
    const double rate = 2.4e6, fc = -400000;
    // a strong carrier 20 kHz away must not reach a 12.5 kHz NFM channel
    float level = 0; bool sq = false;
    RxSettings s;
    s.offsetHz = fc; s.demod = Demod::Nfm; s.squelchDb = -60;
    auto g = [&](double t) { const double a = 2 * M_PI * (fc + 20000) * t; return cf32((float)std::cos(a), (float)std::sin(a)) * 0.5f; };
    auto a = runRx(rate, s, g, 0.4, &level, &sq);
    CHECK(level < -100, "neighbour leaks in: %.1f dBFS", level);
    CHECK(sq, "squelch should be closed");
    double e = 0;
    for (float v : a) e += (double)v * v;
    CHECK(e == 0, "squelched audio must be silent");
    auto g2 = [&](double t) { const double a2 = 2 * M_PI * (fc + 1000) * t; return cf32((float)std::cos(a2), (float)std::sin(a2)) * 0.1f; };
    a = runRx(rate, s, g2, 0.4, &level, &sq);
    CHECK(std::fabs(level - -20) < 1.5, "in-channel level %.1f dBFS (want -20)", level);
    CHECK(!sq, "squelch should be open");
}

static void testWaterfall() {
    Waterfall w;
    CHECK(w.configure(2.4e6, 10), "configure");
    std::vector<cf32> x(2400000);
    for (size_t i = 0; i < x.size(); i++) { const double a = 2 * M_PI * 300000.0 * (double)i / 2.4e6; x[i] = cf32((float)std::cos(a), (float)std::sin(a)) * 0.5f; }
    w.push(x.data(), x.size());
    CHECK(w.seq() >= 9 && w.seq() <= 11, "about 10 rows in 1 s at 10 fps: %llu", (unsigned long long)w.seq());
    std::vector<float> row;
    w.row(-1.2e6, 1.2e6, 1024, row);
    int pk = 0;
    for (int i = 0; i < 1024; i++) if (row[(size_t)i] > row[(size_t)pk]) pk = i;
    const double pkHz = -1.2e6 + (pk + 0.5) * 2.4e6 / 1024;
    CHECK(std::fabs(pkHz - 300000) < 2400, "peak at %.0f Hz", pkHz);
    CHECK(std::fabs(row[(size_t)pk] - -6.0) < 1.6, "peak %.1f dBFS (want -6)", row[(size_t)pk]);
    w.row(290000, 310000, 256, row);   // zoomed in: interpolation, still a peak in the middle
    int pk2 = 0;
    for (int i = 0; i < 256; i++) if (row[(size_t)i] > row[(size_t)pk2]) pk2 = i;
    CHECK(std::abs(pk2 - 128) < 12, "zoomed peak at bin %d", pk2);
    RowScale sc;
    std::vector<uint8_t> q;
    quantizeRow(row, sc, q);
    CHECK(q.size() == 256 && q[(size_t)pk2] > 128, "quantized peak %d", q.empty() ? -1 : (int)q[(size_t)pk2]);
}

static void testRejectsBadConfig() {
    SharedFft f;
    f.configure(200000);
    VirtualRx rx;
    std::string err;
    RxSettings s; s.demod = Demod::Wfm;
    CHECK(!rx.configure(f, s, err), "WFM in a 200 kHz capture must be refused");
    s.demod = Demod::Nfm; s.offsetHz = 150000;
    CHECK(!rx.configure(f, s, err), "a frequency outside the capture must be refused");
}

int main() {
    testSlicePhase();
    testDemods();
    testSelectivityAndSquelch();
    testWaterfall();
    testRejectsBadConfig();
    return testSummary("web_dsp");
}
