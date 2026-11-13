// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Fast waterfall rows (SpectrumAnalyzer::setFastRows): the row count and spacing are exact, keyed signals show up where they are in time,
// the level agrees with the ordinary frames, the queue is bounded, and the frames themselves are untouched.
#include "dect2/dsp/spectrum.h"
#include "dect2/common/stable_random.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }

static std::vector<cf32> keyedTone(double fs, double secs, double toneHz, double onS, double offS, float noise, unsigned seed) {
    std::mt19937 rng(seed);
    dect2::normal_distribution<float> nd(0.f, noise);
    const size_t n = (size_t)(fs * secs);
    std::vector<cf32> x(n);
    for (size_t i = 0; i < n; i++) {
        const double t = i / fs;
        const bool on = std::fmod(t, onS + offS) < onS;
        const double ph = 2 * M_PI * toneHz * t;
        x[i] = cf32((on ? 0.3f * (float)std::cos(ph) : 0.f) + nd(rng), (on ? 0.3f * (float)std::sin(ph) : 0.f) + nd(rng));
    }
    return x;
}

int main() {
    // the transform size
    check(SpectrumAnalyzer::fastRowFft(48000, 100) == 512 * 2 || SpectrumAnalyzer::fastRowFft(48000, 100) == 1024, "48 kHz: the smallest transform (1024)");
    check(SpectrumAnalyzer::fastRowFft(2.4e6, 100) == 32768, "2.4 Msps, 100 rows: 20 ms is 48000 samples -> 32768");
    check(SpectrumAnalyzer::fastRowFft(20e6, 100) == 65536, "20 Msps: capped at 65536");
    check(SpectrumAnalyzer::fastRowFft(2.4e6, 500) <= 16384, "500 rows a second: the cost budget limits the transform");
    check(SpectrumAnalyzer::fastRowFft(0, 100) == 1024 && SpectrumAnalyzer::fastRowFft(48000, 0) == 1024, "nonsense gives the smallest");

    // off by default
    {
        SpectrumAnalyzer an(4096);
        auto x = keyedTone(48000, 1.0, 5000, 0.5, 0.5, 0.01f, 1);
        an.feed(x.data(), x.size());
        std::vector<float> r; size_t bins = 0; double dt = 0; uint64_t dr = 0;
        check(an.takeFastRows(r, bins, dt, dr) == 0, "no rows unless asked for");
    }

    // 48 kHz sound card, 100 rows a second, a tone keyed 0.5 s on / 0.5 s off at +5 kHz
    {
        const double fs = 48000, rps = 100;
        SpectrumAnalyzer an(4096);
        an.setFastRows(rps, fs);
        auto x = keyedTone(fs, 2.0, 5000, 0.5, 0.5, 0.01f, 2);
        for (size_t p = 0; p < x.size(); p += 1000) an.feed(x.data() + p, std::min<size_t>(1000, x.size() - p));   // small pieces, like a sound card
        std::vector<float> rows; size_t bins = 0; double dt = 0; uint64_t dr = 0;
        const size_t n = an.takeFastRows(rows, bins, dt, dr);
        const size_t N = SpectrumAnalyzer::fastRowFft(fs, rps);
        check(bins == N, "the rows have the transform's bins");
        check(std::fabs(dt - 0.01) < 1e-9, "row time is exactly 1/rate");
        const double expect = (fs * 2.0 - N) / (fs / rps) + 1;   // the first row needs a full transform
        check(std::fabs((double)n - expect) <= 2, "row count is right (+-2)");
        check(dr == 0, "nothing dropped");
        // tone bin: DC-centred, bin = N/2 + f/fs*N
        const size_t tb = (size_t)std::llround(N / 2.0 + 5000.0 / fs * N);
        // find the rising edge at t = 1.0 (on 1.0..1.5): the first row whose tone bin is 20 dB over the floor
        auto levelAt = [&](size_t row) { float m = -200; for (size_t k = tb - 1; k <= tb + 1; k++) m = std::max(m, rows[row * bins + k]); return m; };
        auto timeOfRow = [&](size_t row) { return (N + row * (fs / rps)) / fs; };   // the end of the row's transform
        int ons = 0, offs = 0; double firstOn = -1, firstOff = -1;
        for (size_t r = 1; r < n; r++) {
            const bool a = levelAt(r - 1) > -40, b = levelAt(r) > -40;
            if (!a && b) { ons++; if (firstOn < 0 && timeOfRow(r) > 0.8) firstOn = timeOfRow(r); }
            if (a && !b) { offs++; if (firstOff < 0 && timeOfRow(r) > 1.2) firstOff = timeOfRow(r); }
        }
        check(ons == 1 && offs == 2, "in two seconds (0.5 s on, 0.5 s off): the key-down at 1.0 s and the key-ups at 0.5 s and 1.5 s");
        // the window is N/fs long and the row marks its end: key-down at t=1.0 shows by the time the window is partly over it
        check(firstOn > 1.0 && firstOn < 1.0 + (double)N / fs + 0.011, "key-down appears within one window of when it happened");
        check(firstOff > 1.5 && firstOff < 1.5 + (double)N / fs + 0.011, "key-up is seen within one window of when it happened");
        // a second take is empty, then more data gives more rows
        check(an.takeFastRows(rows, bins, dt, dr) == 0, "the queue is empty after a take");
        an.feed(x.data(), 4800);
        check(an.takeFastRows(rows, bins, dt, dr) >= 9, "0.1 s later: about 10 rows");
    }

    // 2.4 Msps, 100 rows: several transforms per row; the level agrees with the ordinary frames
    {
        const double fs = 2.4e6;
        SpectrumAnalyzer a1(32768), a2(32768);
        a2.setFastRows(100, fs);
        auto x = keyedTone(fs, 0.3, 100e3, 1.0, 0.0, 0.05f, 3);
        a1.feed(x.data(), x.size()); a2.feed(x.data(), x.size());
        SpectrumFrame f1, f2;
        check(a1.takeFrame(f1) && a2.takeFrame(f2), "frames still come");
        bool same = f1.dbfs.size() == f2.dbfs.size();
        for (size_t i = 0; same && i < f1.dbfs.size(); i++) same = f1.dbfs[i] == f2.dbfs[i];
        check(same, "fast rows leave the frames bit for bit as they were");
        std::vector<float> rows; size_t bins = 0; double dt = 0; uint64_t dr = 0;
        const size_t n = a2.takeFastRows(rows, bins, dt, dr);
        check(n >= 28 && n <= 31, "0.3 s at 100 rows a second is about 30 rows");
        // noise floor (median of bins away from the tone) of a row against the frame's, in dB: same scale
        auto med = [](std::vector<float> v) { std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; };
        std::vector<float> rr(rows.begin() + (long)((n - 1) * bins), rows.begin() + (long)(n * bins));
        const float mr = med(rr), mf = med(f1.dbfs);
        // different transform lengths: per-bin noise power differs by 10 log10(N1 / N2) only when expressed per bin; both are normalised to total power, so the floors differ by that ratio
        const double expectDiff = 10 * std::log10((double)bins / (double)f1.dbfs.size());   // bins = fastRowFft = 32768 here too, so ~0
        check(std::fabs((mr - mf) - expectDiff) < 1.5, "row level matches the frame level (same scale)");
    }

    // an unread queue is bounded, and says what it dropped
    {
        const double fs = 48000;
        SpectrumAnalyzer an(4096);
        an.setFastRows(500, fs);
        auto x = keyedTone(fs, 60.0, 5000, 0.5, 0.5, 0.01f, 4);
        for (size_t p = 0; p < x.size(); p += 4800) an.feed(x.data() + p, std::min<size_t>(4800, x.size() - p));
        std::vector<float> rows; size_t bins = 0; double dt = 0; uint64_t dr = 0;
        const size_t n = an.takeFastRows(rows, bins, dt, dr);
        check(n > 0 && rows.size() * 4 <= (size_t)9 << 20, "the queue holds at most about 8 MB");
        check(dr > 0, "dropped rows are counted");
    }


    // Overlapped frames: a transform too long for two blocks in a frame period still gives 30 frames a second
    {
        const double fs = 2.4e6;
        const size_t N = 131072;
        auto x = keyedTone(fs, 2.0, 100e3, 1.0, 0.0, 0.05f, 6);
        auto run = [&](bool withRate, std::vector<SpectrumFrame>* keep) {
            SpectrumAnalyzer an(N);
            if (withRate) an.setSampleRate(fs);
            int frames = 0;
            const size_t piece = (size_t)(fs / 30);   // one frame period of signal
            SpectrumFrame f;
            for (size_t p = 0; p + piece <= x.size(); p += piece) {
                an.feed(x.data() + p, piece);
                if (an.takeFrame(f)) { frames++; if (keep) keep->push_back(f); }
            }
            return frames;
        };
        std::vector<SpectrumFrame> fr;
        const int without = run(false, nullptr), with = run(true, &fr);
        check(without >= 30 && without <= 40, "without the sample rate: a frame as often as a block completes (fs / N = 18 a second)");
        check(with >= 56, "with the sample rate: a frame in nearly every one of the 60 periods");
        // the tone at +100 kHz is where it should be, with the same level as in the plain frames
        SpectrumFrame ref;
        { SpectrumAnalyzer an(N); an.feed(x.data(), 2 * N); an.takeFrame(ref); }
        const size_t tb = (size_t)std::llround(N / 2.0 + 100e3 / fs * N);
        const SpectrumFrame& last = fr.back();
        auto peakBin = [&](const SpectrumFrame& f) { size_t b = 0; for (size_t i = 0; i < f.dbfs.size(); i++) if (f.dbfs[i] > f.dbfs[b]) b = i; return b; };
        check(peakBin(last) + 1 >= tb && peakBin(last) <= tb + 1, "the tone sits in its bin");
        check(std::fabs(last.dbfs[peakBin(last)] - ref.dbfs[peakBin(ref)]) < 0.7, "the tone has the same level as in the plain frame");
        auto med = [](std::vector<float> v) { std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; };
        check(std::fabs(med(last.dbfs) - med(ref.dbfs)) < 1.0, "the noise floor is the same (one block against two)");
        // the stats still cover every sample
        check(last.stats.rmsDbfs < 0 && last.stats.rmsDbfs > -40, "the level statistics are there");
    }

    // a small transform is untouched by the sample rate: blocks follow each other, two to a frame
    {
        const double fs = 2.4e6;
        auto x = keyedTone(fs, 0.2, 100e3, 1.0, 0.0, 0.05f, 7);
        SpectrumAnalyzer a1(4096), a2(4096);
        a2.setSampleRate(fs);
        a1.feed(x.data(), x.size()); a2.feed(x.data(), x.size());
        SpectrumFrame f1, f2;
        check(a1.takeFrame(f1) && a2.takeFrame(f2), "frames");
        bool same = f1.dbfs.size() == f2.dbfs.size();
        for (size_t i = 0; same && i < f1.dbfs.size(); i++) same = f1.dbfs[i] == f2.dbfs[i];
        check(same, "4096 bins at 2.4 Msps: bit for bit as before");
    }

    // a changed sample rate or rate restarts the rows cleanly
    {
        SpectrumAnalyzer an(4096);
        an.setFastRows(100, 48000);
        an.setFastRows(100, 96000);
        auto x = keyedTone(96000, 0.5, 5000, 0.5, 0.5, 0.01f, 5);
        an.feed(x.data(), x.size());
        std::vector<float> rows; size_t bins = 0; double dt = 0; uint64_t dr = 0;
        const size_t n = an.takeFastRows(rows, bins, dt, dr);
        check(n >= 45 && n <= 51 && bins == SpectrumAnalyzer::fastRowFft(96000, 100), "new rate: rows at the new hop");
        an.setFastRows(0, 96000);
        an.feed(x.data(), x.size());
        check(an.takeFastRows(rows, bins, dt, dr) == 0, "off again: no rows");
    }
    printf(fails ? "%d FAILED\n" : "spectrum_fastrows: all passed\n", fails);
    return fails ? 1 : 0;
}
