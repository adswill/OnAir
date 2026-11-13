// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB playout (dab_playout.h): concealment without clicks, splices, and the rate control that keeps the output buffer level against a sound card
// whose clock is not the transmitter's.
#include "dect2/dab/dab_playout.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)

static const double kPi = 3.14159265358979323846;

// a 48 kHz stereo tone, block by block
struct Tone {
    double ph = 0, f = 1000, a = 0.5;
    void make(int n, std::vector<float>& v) {
        v.resize((size_t)n * 2);
        for (int i = 0; i < n; i++) { const float s = (float)(a * std::sin(ph)); v[(size_t)i * 2] = s; v[(size_t)i * 2 + 1] = s; ph += 2 * kPi * f / 48000; }
    }
};

// the largest second difference of the left channel: a click is a step, a tone at 1 kHz has 0.5 * (2 pi 1000 / 48000)^2 = 0.0086
static double maxStep(const std::vector<float>& v, size_t from = 2) {
    double m = 0;
    for (size_t i = std::max<size_t>(from, 2); i < v.size() / 2; i++) m = std::max(m, (double)std::fabs(v[i * 2] - 2 * v[(i - 1) * 2] + v[(i - 2) * 2]));
    return m;
}

static void conceal() {
    // the tone in access units of 1920 frames (40 ms); units k are lost: a single one, two in a row, then a long loss of eight (320 ms)
    for (int pattern = 0; pattern < 3; pattern++) {
        DabPlayout p;
        Tone t;
        std::vector<float> out, blk;
        const int lostFrom = 10, lostN = pattern == 0 ? 1 : pattern == 1 ? 2 : 8;
        size_t lostStart = 0, lostEnd = 0;
        for (int k = 0; k < 30; k++) {
            t.make(1920, blk);
            if (k >= lostFrom && k < lostFrom + lostN) { if (k == lostFrom) lostStart = out.size() / 2; p.lost(1920, out); if (k == lostFrom + lostN - 1) lostEnd = out.size() / 2; }
            else p.good(blk.data(), 1920, out);
        }
        const double step = maxStep(out, 4000);
        // the level through the loss: a short one is bridged at full level, a long one fades out (no step) and the tone fades back in
        double minLvl = 1;
        for (size_t i = lostStart; i + 480 <= lostEnd; i += 480) {
            double e = 0;
            for (size_t j = 0; j < 480; j++) e += (double)out[(i + j) * 2] * out[(i + j) * 2];
            minLvl = std::min(minLvl, std::sqrt(e / 480));
        }
        std::printf("loss of %d access units: largest step %.4f (tone 0.0086), lowest level in the loss %.3f (tone 0.354)\n", lostN, step, minLvl);
        CHECK(step < 0.03, "a click: step %.4f", step);
        if (lostN <= 1) CHECK(minLvl > 0.2, "a short loss is bridged at full level: %.3f", minLvl);
        if (lostN >= 8) CHECK(minLvl < 0.001 && p.silentFrames() > 0, "a long loss goes quiet: %.3f", minLvl);
        CHECK(std::abs((long)(out.size() / 2) - 30L * 1920) < 200, "the output keeps time: %zu frames for %d", out.size() / 2, 30 * 1920);
    }
}

// the producer sends 120 ms bursts on the transmitter's clock, the sound card takes 10 ms blocks on its own; the level must stay near the target
static void rate(double ppm) {
    DabPlayout p;
    Tone t;
    std::vector<float> out, blk;
    double q = 0;   // frames queued in the card
    double minQ = 1e9, maxQ = 0;
    int underruns = 0;
    const double seconds = 600;   // ten minutes
    double tCard = 0;
    bool playing = false;
    for (double tx = 0; tx < seconds; tx += 0.12) {
        p.setFill((int)q);
        t.make(5760, blk);
        out.clear();
        p.good(blk.data(), 5760, out);
        q += (double)out.size() / 2;
        // the card plays until the transmitter's time catches up
        while (tCard < tx + 0.12) {
            if (!playing) {   // the card starts once the start level is queued (AudioOut's threshold)
                if (q >= DabPlayout::kTarget) playing = true;
                else { tCard += 0.01; continue; }
            }
            const double take = 480.0 * (1.0 + ppm * 1e-6);
            if (q < take) { underruns++; q = 0; } else q -= take;
            tCard += 0.01;
            if (tx > 60) { minQ = std::min(minQ, q); maxQ = std::max(maxQ, q); }
        }
    }
    std::printf("sound card %+5.0f ppm, 10 minutes: queued %.0f..%.0f frames (target %d), ratio %.6f, %d underruns\n", ppm, minQ, maxQ, DabPlayout::kTarget, p.ratio(), underruns);
    CHECK(underruns == 0 && minQ > 4800 && maxQ < 2.5 * DabPlayout::kTarget, "%+.0f ppm: level %.0f..%.0f, %d underruns", ppm, minQ, maxQ, underruns);
}

int main() {
    conceal();
    for (double ppm : {-1000.0, -100.0, 0.0, 100.0, 1000.0}) rate(ppm);
    if (fails) { std::printf("%d failed\n", fails); return 1; }
    std::printf("all passed\n");
    return 0;
}
