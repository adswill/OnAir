// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tone decoders on an HF voice channel (issue #211): SELCAL sent in USB, as on the aeronautical HF channels, through the USB front end of
// the receiver (setDemod) at a radio's rate with the radio tuned off the channel, with a tuning error, in noise, after speech; LSB and AM
// the same way; the wrong sideband and noise alone give no SELCAL; the auto choice is USB on HF and FM above.
#include "dect2/tones/tones_gen.h"
#include "dect2/tones/tones_proto.h"
#include "dect2/tones/tones_rx.h"
#include "dect2/dsp/portable_rng.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

using namespace dect2;
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

// the analytic signal of the scene's audio (a windowed Hilbert FIR): USB is a + jH{a}, LSB a - jH{a}
static std::vector<std::complex<double>> analytic(const std::vector<float>& a, bool lsb) {
    const int N = 301, m = N / 2;
    std::vector<double> h((size_t)N, 0.0);
    for (int i = 0; i < N; i++) {
        const int k = i - m;
        if (k % 2) h[(size_t)i] = 2.0 / (M_PI * k) * (0.54 + 0.46 * std::cos(M_PI * k / (m + 1)));
    }
    std::vector<std::complex<double>> o(a.size());
    for (size_t n = 0; n < a.size(); n++) {
        double q = 0;
        for (int i = 0; i < N; i++) { const int64_t j = (int64_t)n - (i - m); if (j >= 0 && j < (int64_t)a.size()) q += h[(size_t)i] * a[(size_t)j]; }
        o[n] = {a[n], lsb ? -q : q};
    }
    return o;
}

// the channel at `rate`: the dial at offsetHz in the input (the signal err Hz off), AM: carrier plus audio; complex noise of rms `noise`
static std::vector<cf32> channel(const ToneScene& s, int demod, double rate, double offsetHz, double err, double noise, uint32_t seed) {
    std::vector<std::complex<double>> b;
    if (demod == TonesAm) { b.resize(s.audio().size()); for (size_t i = 0; i < b.size(); i++) b[i] = 1.0 + 0.8 * s.audio()[i]; }
    else b = analytic(s.audio(), demod == TonesLsb);
    const double r0 = s.rate();
    const size_t n = (size_t)((double)b.size() / r0 * rate);
    std::vector<cf32> o(n);
    PortableRng rng(seed);
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / rate, p = t * r0;
        const size_t k = (size_t)p;
        const double f = p - (double)k;
        const std::complex<double> v = k + 1 < b.size() ? b[k] * (1 - f) + b[k + 1] * f : b.back();   // linear: the images land far outside the channel
        const std::complex<double> y = 0.1 * v * std::polar(1.0, 2 * M_PI * (offsetHz + err) * t);
        o[i] = cf32((float)(y.real() + rng.gauss(noise)), (float)(y.imag() + rng.gauss(noise)));
    }
    return o;
}

static TonesTelemetry run(const std::vector<cf32>& iq, int demod, double rate, double offsetHz) {
    TonesReceiver rx;
    rx.setDemod(demod);
    rx.configure(rate);
    rx.setSignalOffset(offsetHz);
    const size_t B = 16384;
    for (size_t i = 0; i < iq.size(); i += B) rx.feed(iq.data() + i, std::min(B, iq.size() - i));
    std::vector<cf32> z((size_t)(1.5 * rate), cf32(0, 0));   // quiet so the last events close
    for (size_t i = 0; i < z.size(); i += B) rx.feed(z.data() + i, std::min(B, z.size() - i));
    TonesTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

static bool hasSelcal(const TonesTelemetry& t, const std::string& want) {
    for (const auto& e : t.events) if (e.kind == TkSelcal && e.text == want) return true;
    return false;
}
static int selcals(const TonesTelemetry& t) { int n = 0; for (const auto& e : t.events) n += e.kind == TkSelcal; return n; }

static void one(const char* what, const char* code, const char* want, int demodTx, int demodRx, double rate, double off, double err, double noise, bool speech, bool expect) {
    ToneScene s(48000, 5);
    if (speech) s.speech(0.2, 1.5, 0.4);
    s.selcal(code, speech ? 2.0 : 0.4, 0.4);
    s.pad(speech ? 5.0 : 3.4);
    const TonesTelemetry t = run(channel(s, demodTx, rate, off, err, noise, 9), demodRx, rate, off);
    std::printf("  %-40s %s: %d SELCAL, first %s, demod %s\n", what, want, selcals(t), selcals(t) ? t.events.back().text.c_str() : "-", tonesDemodName(t.demod));
    if (expect) CHECK(hasSelcal(t, want) && selcals(t) == 1, "%s: SELCAL %s not decoded once (%d)", what, want, selcals(t));
    else CHECK(selcals(t) == 0, "%s: %d SELCAL where none should be", what, selcals(t));
    CHECK(t.demod == demodRx, "%s: telemetry demod %d", what, t.demod);
}

int main() {
    one("USB, 2 Msps, 50 kHz off the channel", "AMCP", "AM-CP", TonesUsb, TonesUsb, 2e6, -50000, 0, 0.002, false, true);
    one("USB, 2 Msps, +15 Hz tuning error, speech", "GHKL", "GH-KL", TonesUsb, TonesUsb, 2e6, -50000, 15, 0.002, true, true);
    one("USB, 250 ksps, -15 Hz, more noise", "BDEF", "BD-EF", TonesUsb, TonesUsb, 250000, 12000, -15, 0.006, false, true);
    one("LSB", "CJMQ", "CJ-MQ", TonesLsb, TonesLsb, 250000, -20000, 0, 0.002, false, true);
    one("AM (VHF airband)", "ABRS", "AB-RS", TonesAm, TonesAm, 250000, 30000, 0, 0.002, false, true);
    one("LSB signal on the USB setting", "AMCP", "AM-CP", TonesLsb, TonesUsb, 250000, -20000, 0, 0.002, false, false);
    {   // noise alone on USB
        ToneScene s(48000, 6); s.pad(6);
        const TonesTelemetry t = run(channel(s, TonesUsb, 250000, 0, 0, 0.01, 4), TonesUsb, 250000, 0);
        CHECK(selcals(t) == 0 && t.events.empty(), "USB noise: %d events", (int)t.events.size());
    }
    CHECK(tonesAutoDemod(8.879e6) == TonesUsb && tonesAutoDemod(145.5e6) == TonesFm && tonesAutoDemod(0) == TonesFm, "auto demod");
    CHECK(tonesTuning().minMhz <= 2.0, "the mode tunes HF");
    std::printf(failures ? "FAILED %d\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
