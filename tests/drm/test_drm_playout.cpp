// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DRM playout (drm_playout.h) on its own: a 440 Hz tone in audio super frames of 400 ms, with lost frames, missing super frames and long outages, played
// by a simulated sound device whose clock runs off the radio's by up to +-300 ppm. Checks: the device never runs empty, its queue stays near the
// target, the resampler finds the clock offset, the joins into and out of a concealment have no steps (clicks), a concealment keeps the level for the
// first 20 ms, the latency comes back after a loss.
#include "dect2/drm/drm_playout.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>
using dect2::drm::Playout;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Sim {
    Playout po;
    double ppm = 0;
    double sig = 0;                // signal time
    double devQ = 0;               // frames queued in the device
    bool playing = false, startedOnce = false;
    int underruns = 0;
    double devMin = 1e9, devMax = 0;
    std::vector<float> heard;      // left channel as the device plays it (what is written, in order)
    uint64_t sfIdx = 0;
    double phase = 0;
    // one super frame of the tone (frames with index in [lostFrom, lostTo) are placeholders)
    void superFrame(size_t lostFrom = 0, size_t lostTo = 0) {
        std::vector<float> x(19200 * 2);
        for (size_t i = 0; i < 19200; i++) { const float v = 0.5f * (float)std::sin(phase); phase += 2 * M_PI * 440.0 / 48000.0; x[2 * i] = v; x[2 * i + 1] = v; }
        std::vector<std::pair<size_t, size_t>> g;
        if (lostTo > lostFrom) g.push_back({lostFrom, lostTo - lostFrom});
        po.decoded(x.data(), 19200, g);
    }
    void skipSuperFrame() { phase += 2 * M_PI * 440.0 / 48000.0 * 19200; }
    // run `secs` of signal in 20 ms steps; a super frame arrives every 400 ms unless drop(index) says it is lost
    void run(double secs, std::function<int(uint64_t)> what = nullptr) {
        const double dt = 0.02;
        const int steps = (int)std::lround(secs / dt);
        for (int s = 0; s < steps; s++) {
            sig += dt;
            if (std::fmod(sig + 1e-9, 0.4) < dt - 1e-9) {
                const int w = what ? what(sfIdx) : 0;   // 0 normal, 1 missing, 2 one audio frame (40 ms) lost in the middle
                if (w == 1) skipSuperFrame(); else if (w == 2) superFrame(9600, 11520); else superFrame();
                sfIdx++;
            }
            std::vector<float> out;
            po.advance(sig, (int)devQ, out);
            for (size_t i = 0; i < out.size(); i += 2) heard.push_back(out[i]);
            devQ += (double)out.size() / 2;
            if (!playing && devQ >= Playout::kDeviceStartFrames) { playing = true; startedOnce = true; }
            if (playing) {
                const double use = dt * 48000.0 * (1 + ppm * 1e-6);
                if (devQ < use) { underruns++; devQ = 0; playing = false; }
                else devQ -= use;
                if (sig > 30) { devMin = std::min(devMin, devQ); devMax = std::max(devMax, devQ); }
            }
        }
    }
};

// largest step between neighbouring samples in heard[from, to)
static double maxStep(const std::vector<float>& h, size_t from, size_t to) {
    double m = 0;
    for (size_t i = std::max<size_t>(from, 1); i < to && i < h.size(); i++) m = std::max(m, (double)std::fabs(h[i] - h[i - 1]));
    return m;
}

int main() {
    const double toneStep = 0.5 * 2 * M_PI * 440.0 / 48000.0;   // the largest step of the tone itself
    // clock drift: 10 minutes at each offset
    for (double ppm : {-300.0, -100.0, 0.0, 100.0, 300.0}) {
        Sim s; s.ppm = ppm;
        s.po.reset();
        s.run(600);
        const double drift = (s.po.ratio() - 1) * 1e6;
        printf("device %+5.0f ppm: underruns %d  device queue %.3f .. %.3f s  resampler %+7.1f ppm  made up %.2f s  trimmed %.2f s\n", ppm, s.underruns, s.devMin / 48000, s.devMax / 48000,
               drift, s.po.concealedSecs() + s.po.mutedSecs(), s.po.trimmedSecs());
        CHECK(s.underruns == 0, "%+.0f ppm: %d underruns", ppm, s.underruns);
        CHECK(s.devMin > 0.05 * 48000 && s.devMax < 0.4 * 48000, "%+.0f ppm: device queue %.3f .. %.3f s", ppm, s.devMin / 48000, s.devMax / 48000);
        CHECK(std::fabs(drift - ppm) < 30, "%+.0f ppm: the resampler runs at %+.1f ppm", ppm, drift);
        CHECK(s.po.concealedSecs() + s.po.mutedSecs() < 0.01, "%+.0f ppm: %.3f s made up without a loss", ppm, s.po.concealedSecs() + s.po.mutedSecs());
        CHECK(maxStep(s.heard, 48000 * 5, s.heard.size()) < 1.5 * toneStep, "%+.0f ppm: a step of %.4f in the sound (the tone has %.4f)", ppm, maxStep(s.heard, 48000 * 5, s.heard.size()), toneStep);
    }
    // a lost audio frame (40 ms) every 2 s, a missing super frame every 7 s, at +80 ppm
    {
        Sim s; s.ppm = 80;
        int missing = 0, lostFrames = 0;
        s.run(120, [&](uint64_t i) { const int w = i > 10 && i % 17 == 0 ? 1 : i > 10 && i % 5 == 0 ? 2 : 0; missing += w == 1; lostFrames += w == 2; return w; });
        const double made = s.po.concealedSecs() + s.po.mutedSecs();
        const double lost = missing * 0.4 + lostFrames * 0.04;
        const double step = maxStep(s.heard, 48000 * 5, s.heard.size());
        printf("losses: underruns %d  made up %.2f s (%.2f s muted) in %llu concealments  largest step %.4f (tone %.4f)  queue %.2f s\n", s.underruns, made, s.po.mutedSecs(),
               (unsigned long long)s.po.events(), step, toneStep, s.po.queueSecs());
        CHECK(s.underruns == 0, "losses: %d underruns", s.underruns);
        // exactly what was lost is made up (the latency stays)
        printf("  lost %.2f s (%d super frames, %d audio frames)\n", lost, missing, lostFrames);
        CHECK(std::fabs(made - lost) < 0.05, "losses: %.2f s made up for %.2f s lost", made, lost);
        CHECK(step < 4 * toneStep, "losses: a step of %.4f at a join (the tone has %.4f)", step, toneStep);
    }
    // the level in the first 20 ms of a concealment of a steady tone
    {
        Sim s;
        s.run(20);
        s.run(1.2, [](uint64_t) { return 1; });
        // 10 ms blocks over the loss: the tone (rms 0.354) goes on, fades over about 200 ms (blocks between full and silent) and comes back; never a
        // hard cut from full level to silence
        int full = 0, between = 0, silentB = 0;
        const size_t from = (size_t)(19.5 * 48000);
        for (size_t b = from; b + 480 <= s.heard.size(); b += 480) {
            double e = 0; for (size_t i = 0; i < 480; i++) e += (double)s.heard[b + i] * s.heard[b + i];
            const double r = std::sqrt(e / 480);
            if (r > 0.33) full++; else if (r > 0.02) between++; else silentB++;
        }
        printf("concealment of a tone (3 super frames lost): 10 ms blocks at full level %d, fading %d, silent %d\n", full, between, silentB);
        CHECK(between >= 12 && between <= 30, "concealment: %d blocks between full level and silence (a fade of 200 ms has about 20)", between);
        // the latency is back: the queue at its nominal level after the loss (one super frame and the margin, less what has been played since)
        s.run(4);
        printf("queue after recovery %.3f s\n", s.po.queueSecs());
        CHECK(s.po.queueSecs() > 0.2 && s.po.queueSecs() < 0.7, "queue %.3f s after the loss", s.po.queueSecs());
        CHECK(s.underruns == 0, "outage: %d underruns", s.underruns);
    }
    // a 5 s outage: silence (faded), the device keeps playing, the sound comes back
    {
        Sim s;
        s.run(20);
        s.run(5, [](uint64_t) { return 1; });
        s.run(10);
        printf("5 s outage: underruns %d  concealed %.2f s  muted %.2f s  real %.1f s\n", s.underruns, s.po.concealedSecs(), s.po.mutedSecs(), s.po.realSecs());
        CHECK(s.underruns == 0, "5 s outage: %d underruns", s.underruns);
        CHECK(s.po.mutedSecs() > 4.0 && s.po.concealedSecs() < 0.4, "5 s outage: %.2f s muted, %.2f s concealed", s.po.mutedSecs(), s.po.concealedSecs());
        CHECK(maxStep(s.heard, 48000 * 5, s.heard.size()) < 4 * toneStep, "5 s outage: a step of %.4f", maxStep(s.heard, 48000 * 5, s.heard.size()));
    }
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
