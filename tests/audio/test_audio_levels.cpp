// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sound level: the volume taper, the leveller, and the level each main mode hands to the output (measured on the synthetic signals, no sound is played).
#include "dect2/audio/audiolevel.h"
#include "dect2/airband/airband_gen.h"
#include "dect2/airband/airband_rx.h"
#include "dect2/fm/fm_gen.h"
#include "dect2/fm/fm_rx.h"
#include "dect2/general/general_gen.h"
#include "dect2/general/general_rx.h"
#include "dect2/source/source.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static double db(double x) { return 20 * std::log10(std::max(x, 1e-9)); }

// mono audio at `rate` -> stereo 48 kHz (sample repeat), the leveller, then the volume taper; returns RMS and peak (dBFS) of the last 2 s
struct Lv { double rms, peak; };
static Lv measure(const std::vector<float>& mono, int rate, bool level, float vol) {
    const int rep = 48000 / rate;
    std::vector<float> st;
    st.reserve(mono.size() * rep * 2);
    for (float v : mono) for (int k = 0; k < rep; k++) { st.push_back(v); st.push_back(v); }
    if (level) { AudioLeveler lv; lv.process(st.data(), (int)(st.size() / 2)); }
    const float g = volumeTaper(vol);
    const size_t from = st.size() > 4 * 48000 ? st.size() - 4 * 48000 : 0;   // last 2 s
    double s = 0, pk = 0;
    for (size_t i = from; i < st.size(); i++) { const double v = st[i] * g; s += v * v; pk = std::max(pk, std::fabs(v)); }
    return {db(std::sqrt(s / (st.size() - from))), db(pk)};
}

static void row(const char* name, const std::vector<float>& a, int rate) {
    if (a.size() < (size_t)rate * 3) { printf("  %-14s no audio (%zu samples)\n", name, a.size()); return; }
    const Lv b100 = measure(a, rate, false, 1.f), b1 = measure(a, rate, false, 0.01f);   // before: linear volume
    const Lv a100 = measure(a, rate, true, 1.f), a1 = measure(a, rate, true, 0.01f);
    printf("  %-14s before 100%%: rms %6.1f pk %6.1f | 1%%: rms %6.1f | after 100%%: rms %6.1f pk %6.1f | 1%%: rms %6.1f\n",
           name, b100.rms, b100.peak, b1.rms, a100.rms, a100.peak, a1.rms);
    CHECK(a100.peak <= 0.001, "%s: the limiter keeps the peaks below full scale (%.1f dBFS)", name, a100.peak);
    CHECK(a100.rms <= kNominalRmsDb + 1.5, "%s: leveled to the nominal level (%.1f dBFS)", name, a100.rms);
}

int main() {
    // ---- taper
    CHECK(volumeTaper(0.f) == 0.f && volumeTaper(-1.f) == 0.f, "0 is silence");
    CHECK(std::fabs(volumeTaper(1.f) - 1.f) < 1e-6f && volumeTaper(2.f) == 1.f, "full is 0 dB");
    float prev = 0.f; bool mono = true;
    for (int i = 1; i <= 100; i++) { const float g = volumeTaper(i / 100.f); if (!(g > prev)) mono = false; prev = g; }
    CHECK(mono, "the taper rises with every step");
    CHECK(db(volumeTaper(0.01f)) <= -40, "the first step is quiet (%.1f dB)", db(volumeTaper(0.01f)));
    CHECK(db(volumeTaper(0.5f)) < -20 && db(volumeTaper(0.5f)) > -30, "half is about -25 dB (%.1f)", db(volumeTaper(0.5f)));
    printf("  taper: 1%% %.1f dB, 10%% %.1f dB, 50%% %.1f dB, 100%% %.1f dB\n", db(volumeTaper(0.01f)), db(volumeTaper(0.1f)), db(volumeTaper(0.5f)), db(volumeTaper(1.f)));

    // ---- leveller: loud is turned down to the nominal level, quiet is left alone, peaks never pass full scale
    {
        std::vector<float> loud(48000 * 6 * 2), quiet(48000 * 4 * 2);
        for (size_t i = 0; i < loud.size() / 2; i++) loud[2 * i] = loud[2 * i + 1] = 0.9f * (float)std::sin(0.13 * (double)i);
        for (size_t i = 0; i < quiet.size() / 2; i++) quiet[2 * i] = quiet[2 * i + 1] = 0.01f * (float)std::sin(0.13 * (double)i);
        AudioLeveler a, b;
        a.process(loud.data(), (int)(loud.size() / 2)); b.process(quiet.data(), (int)(quiet.size() / 2));
        double s = 0, pk = 0, sq = 0;
        for (size_t i = loud.size() - 96000; i < loud.size(); i++) { s += loud[i] * loud[i]; pk = std::max(pk, (double)std::fabs(loud[i])); }
        for (size_t i = quiet.size() - 96000; i < quiet.size(); i++) sq += quiet[i] * quiet[i];
        const double r = db(std::sqrt(s / 96000)), rq = db(std::sqrt(sq / 96000));
        CHECK(std::fabs(r - kNominalRmsDb) < 1.0, "loud tone leveled to %.1f dBFS", r);
        CHECK(pk <= 1.0, "peak %.2f", pk);
        CHECK(std::fabs(rq - db(0.01 / std::sqrt(2.0))) < 0.2, "a quiet tone is not boosted (%.1f dBFS)", rq);
        float x[2] = {3.f, -3.f}; AudioLeveler c; c.process(x, 1);
        CHECK(std::fabs(x[0]) <= 1.f && std::fabs(x[1]) <= 1.f, "an overload is limited");
    }

    // ---- level each mode hands to the output (before the volume)
    printf("  levels in dBFS (before = old linear slider, after = leveler + taper)\n");
    {   // FM broadcast, stereo tones at the generator's default amplitude
        FmGenConfig c; c.rate = 2e6; c.cnrDb = 45;
        FmGenerator gen(c); FmReceiver rx; rx.setSilent(true); rx.configure(c.rate);
        std::vector<float> m;
        rx.setAudioTap([&](const float* l, const float* r, size_t n) { for (size_t i = 0; i < n; i++) m.push_back(0.5f * (l[i] + r[i])); });
        std::vector<cf32> buf; const size_t total = (size_t)(6 * c.rate);
        for (size_t d = 0; d < total; d += 65536) { buf.clear(); gen.generate(std::min<size_t>(65536, total - d), buf); rx.feed(buf.data(), buf.size()); }
        row("FM broadcast", m, 48000);
    }
    {   // airband AM, the ATIS channel (continuous speech-like audio) of the test layout, 8 kHz
        const double kRate = 1e6, kDial = 118.700e6, kOff = airbandTuning().tuneOffsetHz;
        const auto layout = airbandTestLayout(); const auto list = airbandChannelsFor(layout, kDial);
        AirbandGenConfig g; g.chans = layout; g.snrDb = 25; g.rate = kRate; g.dialOffsetHz = -kOff;
        AirbandGenerator gen(g);
        std::vector<cf32> x((size_t)(10 * kRate));
        for (size_t o = 0; o < x.size(); o += 65536) gen.generate(x.data() + o, std::min<size_t>(65536, x.size() - o));
        AirbandReceiver rx; rx.setSilent(true); rx.configure(kRate); rx.setCenterHz(kDial + kOff); rx.setChannels(list);
        std::vector<std::vector<float>> aud(list.size());
        rx.setChannelTap([&](int c, const float* a, size_t n) { aud[(size_t)c].insert(aud[(size_t)c].end(), a, a + n); });
        for (size_t o = 0; o < x.size(); o += 8192) rx.feed(x.data() + o, std::min<size_t>(8192, x.size() - o));
        row("airband AM", aud[4], 8000);
    }
    const struct { const char* name; int d, m; } gm[] = {{"general demod0", 0, 0}, {"general demod1", 1, 1}, {"general demod2", 2, 2}, {"general demod3", 3, 3}};
    for (const auto& q : gm) {
        const double kRate = 2e6;
        SynthConfig sc; sc.mode = 57; sc.snrDb = 30; sc.modeOpt[0] = q.m; sc.modeOpt[1] = 0; sc.modeVal[0] = 0; sc.modeVal[1] = 1000;
        auto syn = makeGeneralSynth(sc, kRate);
        GeneralReceiver r; r.configure(kRate); r.setSignalOffset(-generalTuning().tuneOffsetHz); r.setSilent(true);
        r.setDemod(q.d); r.setWidthHz(generalDefaultWidth(q.d)); r.setSquelch(false, false, -60);
        std::vector<float> m; r.setAudioTap([&](const float* x, size_t n) { m.insert(m.end(), x, x + n); });
        std::vector<cf32> buf(20000);
        for (double t = 0; t < 6.0; t += buf.size() / kRate) { syn->generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
        row(q.name, m, 48000);
    }
    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
