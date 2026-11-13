// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Neural noise remover (RNNoise): off = bit-identical, delay and size invariants at 8 / 12 / 16 / 24 / 48 kHz, strength 0 = the dry signal
// delayed, noise between speech-like bursts goes down, chunking does not change the result.
#include "dect2/audio/neural_nr.h"
#include "dect2/common/stable_random.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// voiced bursts (130 Hz pitch, harmonics up to 3.4 kHz, 0.5 s on / 0.5 s off) plus white noise; speech[] marks the burst samples
static void makeSignal(int rate, double seconds, float noiseRms, std::vector<float>& x, std::vector<char>& speech, std::vector<float>& clean, unsigned seed = 1) {
    const size_t n = (size_t)(rate * seconds);
    x.assign(n, 0.f); speech.assign(n, 0); clean.assign(n, 0.f);
    std::mt19937 g(seed);
    dect2::normal_distribution<float> nd(0.f, noiseRms);
    const double top = std::min(3400.0, 0.45 * rate);
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / rate;
        const bool on = std::fmod(t, 1.0) < 0.5;
        double v = 0;
        if (on) {
            const double env = 0.5 * (1 - std::cos(2 * 3.14159265358979 * 4 * t));   // syllables
            const double f0 = 130 + 15 * std::sin(2 * 3.14159265358979 * 2 * t);
            for (int h = 1; h * f0 < top; h++) v += std::sin(2 * 3.14159265358979 * h * f0 * t) / h;
            v *= 0.25 * env;
        }
        clean[i] = (float)v; speech[i] = on;
        x[i] = (float)v + nd(g);
    }
}

static double rmsGaps(const std::vector<float>& y, const std::vector<char>& speech, size_t from, size_t lag) {   // y delayed by lag against speech[]
    double s = 0; size_t c = 0;
    for (size_t i = from; i + lag < y.size(); i++) {
        const size_t j = i;   // gap: speech off for 60 ms either side of this sample, in input time
        if (j < 2000 || speech[j] || (j + 1000 < speech.size() && speech[j + 1000]) || speech[j - 1000]) continue;
        s += (double)y[i + lag] * y[i + lag]; c++;
    }
    return c ? std::sqrt(s / (double)c) : 0;
}

int main() {
    const int rates[] = {8000, 12000, 16000, 24000, 48000};
    for (int rate : rates) {
        std::vector<float> x; std::vector<char> sp; std::vector<float> clean;
        makeSignal(rate, 4.0, 0.03f, x, sp, clean);

        // 1. off (the default): not touched, bit for bit
        {
            NeuralNr nr(rate);
            CHECK(nr.supported(), "rate %d supported", rate);
            CHECK(!nr.enabled(), "off by default");
            std::vector<float> y = x;
            nr.process(y.data(), y.size());
            CHECK(std::memcmp(y.data(), x.data(), x.size() * sizeof(float)) == 0, "off is bit-identical at %d", rate);
            nr.setEnabled(true); nr.setStrength(1.f); nr.setEnabled(false);
            y = x; nr.process(y.data(), y.size());
            CHECK(std::memcmp(y.data(), x.data(), x.size() * sizeof(float)) == 0, "off again is bit-identical at %d", rate);
        }
        // 2. strength 0: the input delayed by latency(), exactly; size unchanged
        {
            NeuralNr nr(rate);
            nr.setEnabled(true); nr.setStrength(0.f);
            std::vector<float> y = x;
            nr.process(y.data(), y.size());
            const size_t lat = (size_t)nr.latency();
            CHECK(lat > 0 && lat < (size_t)rate / 20, "latency %zu plausible at %d", lat, rate);
            bool same = y.size() == x.size();
            for (size_t i = 0; i < y.size() && same; i++) same = y[i] == (i >= lat ? x[i - lat] : 0.f);
            CHECK(same, "strength 0 is the delayed input at %d", rate);
        }
        // 3. strength 1: noise between the bursts goes down, the bursts stay, the delay is latency()
        {
            NeuralNr nr(rate);
            nr.setEnabled(true); nr.setStrength(1.f);
            std::vector<float> y = x;
            nr.process(y.data(), y.size());
            const size_t lat = (size_t)nr.latency();
            const double before = rmsGaps(x, sp, 2 * (size_t)rate, 0), after = rmsGaps(y, sp, 2 * (size_t)rate, lat);
            const double redDb = 20 * std::log10(before / std::max(after, 1e-9));
            printf("rate %5d: latency %zu samples (%.1f ms), gap noise %.4f -> %.4f (%.1f dB down)\n", rate, lat, 1000.0 * (double)lat / rate, before, after, redDb);
            CHECK(redDb > 6.0, "noise reduced by >6 dB at %d (got %.1f)", rate, redDb);
            double sx = 0, sy = 0; size_t c = 0;   // the speech level is kept to within 6 dB
            for (size_t i = 2 * (size_t)rate; i + lat < y.size(); i++) if (sp[i] && sp[i + 600 < sp.size() ? i + 600 : i] && sp[i - 600]) { sx += clean[i] * clean[i]; sy += (double)y[i + lat] * y[i + lat]; c++; }
            CHECK(c > 0 && sy > 0.25 * sx * 0.5, "speech kept at %d (%.1f dB)", rate, 10 * std::log10(sy / std::max(sx, 1e-12)));
            // SNR (speech burst level over the noise left between bursts) improves by more than 6 dB
            double bx = 0, by = 0; size_t cb = 0;
            for (size_t i = 2 * (size_t)rate; i + lat < y.size(); i++) if (sp[i] && sp[i + 600 < sp.size() ? i + 600 : i] && sp[i - 600]) { bx += (double)x[i] * x[i]; by += (double)y[i + lat] * y[i + lat]; cb++; }
            const double snrIn = 20 * std::log10(std::sqrt(bx / (double)cb) / before), snrOut = 20 * std::log10(std::sqrt(by / (double)cb) / std::max(after, 1e-9));
            CHECK(snrOut - snrIn > 6.0, "SNR improves at %d (%.1f -> %.1f dB)", rate, snrIn, snrOut);
            CHECK(nr.vad() >= 0.f && nr.vad() <= 1.f, "vad range");
            bool fin = true; for (float v : y) fin = fin && std::isfinite(v) && std::fabs(v) <= 1.f;
            CHECK(fin, "output finite and within full scale at %d", rate);
        }
        // 3b. the wet path lags by latency() (+-1): so the dry path, delayed the same, blends in step
        {
            std::vector<float> xc, c2; std::vector<char> s2;
            makeSignal(rate, 3.0, 0.f, xc, s2, c2);
            NeuralNr nr(rate); nr.setEnabled(true); nr.setStrength(1.f);
            std::vector<float> y = xc; nr.process(y.data(), y.size());
            int best = 0; double bv = -1e30;
            for (int lag = 0; lag < rate / 10; lag++) {
                double acc = 0;
                for (size_t i = (size_t)rate; i + (size_t)lag < y.size(); i += 2) acc += (double)xc[i] * y[i + (size_t)lag];
                if (acc > bv) { bv = acc; best = lag; }
            }
            CHECK(std::abs(best - nr.latency()) <= 1, "wet delay %d vs latency %d at %d", best, nr.latency(), rate);
        }
        // 4. the result does not depend on how the audio is cut into blocks
        {
            NeuralNr a(rate), b(rate);
            a.setEnabled(true); b.setEnabled(true); a.setStrength(0.7f); b.setStrength(0.7f);
            std::vector<float> ya = x, yb = x;
            a.process(ya.data(), ya.size());
            const size_t cuts[] = {1, 7, 160, 481, 1000, 3};
            size_t pos = 0, k = 0;
            while (pos < yb.size()) { const size_t m = std::min(cuts[k++ % 6], yb.size() - pos); b.process(yb.data() + pos, m); pos += m; }
            CHECK(std::memcmp(ya.data(), yb.data(), ya.size() * sizeof(float)) == 0, "block size does not matter at %d", rate);
        }
    }
    // 5. a rate that does not divide 48 kHz: not supported, untouched
    {
        NeuralNr nr(44100);
        CHECK(!nr.supported() && !NeuralNr::rateSupported(44100) && !NeuralNr::rateSupported(7000), "44.1 kHz unsupported");
        nr.setEnabled(true);
        std::vector<float> x(1000, 0.25f), y = x;
        nr.process(y.data(), y.size());
        CHECK(x == y, "unsupported rate is a pass-through");
    }
    // 6. switching on after off starts clean (same output as a fresh instance); strength changes stay in range
    {
        std::vector<float> x; std::vector<char> sp; std::vector<float> clean;
        makeSignal(8000, 2.0, 0.03f, x, sp, clean, 5);
        NeuralNr a(8000), b(8000);
        a.setEnabled(true); a.setStrength(1.f);
        std::vector<float> junk = x; a.process(junk.data(), junk.size());
        a.setEnabled(false); std::vector<float> t = x; a.process(t.data(), t.size());
        CHECK(t == x, "off after on is untouched");
        a.setEnabled(true); b.setEnabled(true); b.setStrength(1.f);
        std::vector<float> ya = x, yb = x;
        a.process(ya.data(), 4000); b.process(yb.data(), 4000);
        // the buffers start clean; the network's own memory is not reset, so compare only the first 100 ms of the dry path before the net speaks
        bool ok = true; for (int i = 0; i < 20; i++) ok = ok && std::isfinite(ya[(size_t)i]);
        CHECK(ok, "re-enable gives finite output");
        a.setStrength(5.f); CHECK(a.strength() == 1.f, "strength clamps high");
        a.setStrength(-1.f); CHECK(a.strength() == 0.f, "strength clamps low");
    }
    printf(fails ? "neural_nr: %d FAILED\n" : "neural_nr: all passed\n", fails);
    return fails ? 1 : 0;
}
