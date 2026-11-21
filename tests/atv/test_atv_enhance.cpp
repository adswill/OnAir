// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Analog TV video enhancer (core/include/dect2/atv/atv_enhance.h): synthetic PAL (768x576) and NTSC (640x480) pictures with a test pattern.
//   noise power drops (Light and Full), edges stay where they belong after scaling, the aspect is the asked one, a moving object leaves
//   no ghost, a woven moving edge loses its comb, the same input gives the same bytes. Prints the time per picture for the record.
#include "dect2/atv/atv_enhance.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace dect2;
using Buf = std::vector<uint8_t>;

static int g_bad = 0;
#define CHECK(c, ...) do { if (!(c)) { g_bad++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Rng {
    uint32_t s = 12345;
    uint32_t next() { s = s * 1664525u + 1013904223u; return s >> 8; }
    double gauss() { double a = 0; for (int i = 0; i < 12; i++) a += (next() & 0xFFFF) / 65536.0; return a - 6.0; }   // ~N(0,1)
};

static void put(Buf& b, int w, int x, int y, int r, int g, int bl) {
    uint8_t* p = &b[((size_t)y * w + x) * 4];
    p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)bl; p[3] = 255;
}

// grey gradient, a white bar (x in [w/4, w*3/8)), a red block, a dark circle: sharp edges to check geometry
static Buf pattern(int w, int h) {
    Buf b((size_t)w * h * 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int v = 60 + 60 * x / w;
        int r = v, g = v, bl = v;
        if (x >= w / 4 && x < w * 3 / 8 && y < h / 2) r = g = bl = 235;
        if (x >= w / 2 && x < w * 3 / 4 && y >= h / 2 && y < h * 3 / 4) { r = 200; g = 40; bl = 40; }
        const int cx = w * 3 / 4, cy = h / 4, rr = h / 8;
        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) < rr * rr) r = g = bl = 25;
        put(b, w, x, y, r, g, bl);
    }
    return b;
}

static Buf addNoise(const Buf& in, double sigma, Rng& rng) {
    Buf o = in;
    for (size_t i = 0; i < o.size(); i += 4) {
        const double nz = rng.gauss() * sigma;
        for (int j = 0; j < 3; j++) o[i + j] = (uint8_t)std::min(255.0, std::max(0.0, o[i + j] + nz));   // luma-like noise on all three
    }
    return o;
}

static double mse(const Buf& a, const Buf& b) {
    double s = 0; size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4) for (int j = 0; j < 3; j++) { const double d = (double)a[i + j] - b[i + j]; s += d * d; n++; }
    return s / n;
}

static int lumaAt(const Buf& b, int w, int x, int y) { const uint8_t* p = &b[((size_t)y * w + x) * 4]; return (p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8; }

static void noiseTest(const char* name, int w, int h, AtvEnhLevel lv, double maxRatio) {
    const Buf clean = pattern(w, h);
    Rng rng;
    AtvEnhancer e;
    AtvEnhParams p; p.level = lv;
    Buf out; int ow = 0, oh = 0;
    double before = 0, after = 0;
    for (int i = 0; i < 12; i++) {
        const Buf noisy = addNoise(clean, 9.0, rng);
        CHECK(e.process(noisy.data(), w, h, p, out, ow, oh), "%s: process", name);
        before = mse(noisy, clean);
        if (i >= 8) after = mse(out, clean);
    }
    CHECK(ow == w && oh == h, "%s: as received keeps the size (%dx%d)", name, ow, oh);
    std::printf("%s: noise power %.1f -> %.1f (x%.2f), sigma estimate %.1f\n", name, before, after, after / before, e.noiseSigma());
    CHECK(after < before * maxRatio, "%s: noise power must drop below x%.2f (got x%.2f)", name, maxRatio, after / before);
}

static void geometryTest(const char* name, int w, int h, double aspect, int outH, AtvEnhLevel lv) {
    const Buf clean = pattern(w, h);
    AtvEnhancer e;
    AtvEnhParams p; p.level = lv; p.outHeight = outH; p.displayAspect = aspect;
    Buf out; int ow = 0, oh = 0;
    for (int i = 0; i < 3; i++) e.process(clean.data(), w, h, p, out, ow, oh);
    const int wantH = outH ? outH : h;
    const int wantW = outH ? (int)std::lround(outH * aspect / 2.0) * 2 : w;
    CHECK(oh == wantH && ow == wantW, "%s: output %dx%d, wanted %dx%d", name, ow, oh, wantW, wantH);
    CHECK(out.size() == (size_t)ow * oh * 4, "%s: buffer size", name);
    // the left edge of the white bar (source x = w/4) must land at ow/4 within 1.5 px, and the top-of-pattern row stays in place
    const int row = oh / 8;
    double edge = -1;
    for (int x = 1; x < ow; x++) {
        const int a = lumaAt(out, ow, x - 1, row), b = lumaAt(out, ow, x, row);
        if (a < 150 && b >= 150) { edge = (x - 1) + (150.0 - a) / (b - a); break; }
    }
    const double want = ((double)w / 4 * ow) / w;
    CHECK(edge > 0 && std::fabs(edge - want) < 1.5, "%s: bar edge at %.2f, wanted %.2f", name, edge, want);
    // vertical: the bar ends at source y = h/2
    const int col = ow * 5 / 16;
    double vedge = -1;
    for (int y = 1; y < oh; y++) {
        const int a = lumaAt(out, ow, col, y - 1), b = lumaAt(out, ow, col, y);
        if (a >= 150 && b < 150) { vedge = (y - 1) + (a - 150.0) / (a - b); break; }
    }
    const double vwant = (double)(h / 2) * oh / h;
    CHECK(vedge > 0 && std::fabs(vedge - vwant) < 1.5, "%s: bar bottom at %.2f, wanted %.2f", name, vedge, vwant);
    // sharp: the edge must not be smeared over more than a few pixels, and no overshoot beyond the pattern range by more than 12 levels
    int lo = 255, hi = 0;
    for (int x = 0; x < ow; x++) { const int v = lumaAt(out, ow, x, row); lo = std::min(lo, v); hi = std::max(hi, v); }
    CHECK(hi <= 235 + 12 && lo >= 60 - 12, "%s: row range %d..%d (ringing)", name, lo, hi);
}

static void ghostTest(const char* name, int w, int h, AtvEnhLevel lv) {
    // a bright square moving 14 px per picture over a dark flat background, light noise
    AtvEnhancer e;
    AtvEnhParams p; p.level = lv;
    Rng rng;
    Buf out; int ow = 0, oh = 0;
    const int sz = 60, y0 = h / 2 - sz / 2, step = 14;
    int worst = 0;
    for (int i = 0; i < 20; i++) {
        Buf f((size_t)w * h * 4);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) put(f, w, x, y, 40, 40, 40);
        const int x0 = 40 + i * step;
        for (int y = y0; y < y0 + sz; y++) for (int x = x0; x < x0 + sz; x++) put(f, w, x, y, 230, 230, 230);
        f = addNoise(f, 2.0, rng);
        e.process(f.data(), w, h, p, out, ow, oh);
        if (i >= 4) {   // the strip the square just left: the 3 columns behind its trailing edge, well inside the square's rows
            for (int y = y0 + 8; y < y0 + sz - 8; y++) for (int x = x0 - 3 * step; x < x0 - 2; x++) worst = std::max(worst, std::abs(lumaAt(out, ow, x, y) - 40));
        }
    }
    std::printf("%s: worst trail deviation %d levels\n", name, worst);
    CHECK(worst <= 14, "%s: ghost trail %d levels behind a moving object", name, worst);
}

static void combTest(int w, int h) {
    // a woven picture of a moving bar: odd lines are shifted by 10 px against even ones (the comb); Full must pull the sides together
    AtvEnhancer e;
    AtvEnhParams p; p.level = AtvEnhLevel::Full;
    Buf out; int ow = 0, oh = 0;
    double combBefore = 0, combAfter = 0;
    for (int i = 0; i < 6; i++) {
        Buf f((size_t)w * h * 4);
        const int xa = 100 + i * 20;
        for (int y = 0; y < h; y++) {
            const int x0 = xa + (y & 1) * 10;
            for (int x = 0; x < w; x++) { const int v = (x >= x0 && x < x0 + 80) ? 220 : 50; put(f, w, x, y, v, v, v); }
        }
        e.process(f.data(), w, h, p, out, ow, oh);
        if (i == 5) {
            auto comb = [&](const Buf& b) { double s = 0; for (int y = h / 3; y < 2 * h / 3; y++) for (int x = 0; x < w; x++) s += std::abs(lumaAt(b, w, x, y) - (lumaAt(b, w, x, y - 1) + lumaAt(b, w, x, y + 1)) / 2); return s; };
            combBefore = comb(f); combAfter = comb(out);
        }
    }
    std::printf("comb energy %.0f -> %.0f\n", combBefore, combAfter);
    CHECK(combAfter < combBefore * 0.6, "comb repair: %.0f -> %.0f", combBefore, combAfter);
}

static void determinismAndOff() {
    const int w = 320, h = 240;
    const Buf clean = pattern(w, h);
    AtvEnhParams p; p.level = AtvEnhLevel::Full; p.outHeight = 720;
    Buf o1, o2; int a = 0, b = 0;
    for (int run = 0; run < 2; run++) {
        AtvEnhancer e; Rng rng; Buf out;
        for (int i = 0; i < 5; i++) { const Buf n = addNoise(clean, 6, rng); e.process(n.data(), w, h, p, out, a, b); }
        (run ? o2 : o1) = out;
    }
    CHECK(!o1.empty() && o1 == o2, "same input sequence must give the same bytes");
    AtvEnhancer e; Buf out; int ow = 0, oh = 0;
    AtvEnhParams off; off.level = AtvEnhLevel::Off;
    CHECK(!e.process(clean.data(), w, h, off, out, ow, oh), "Off does nothing");
    // reset() forgets the history: the first picture after it equals a fresh enhancer's first picture
    AtvEnhancer f1, f2; Buf x1, x2;
    AtvEnhParams lp; lp.level = AtvEnhLevel::Light;
    f1.process(clean.data(), w, h, lp, x1, ow, oh);
    Rng rng20;
    f2.process(addNoise(clean, 20, rng20).data(), w, h, lp, x2, ow, oh);
    f2.reset();
    f2.process(clean.data(), w, h, lp, x2, ow, oh);
    CHECK(x1 == x2, "reset forgets the history");
}

static void timing(const char* name, int w, int h, AtvEnhLevel lv, int outH) {
    const Buf f = pattern(w, h);
    Rng rng;
    AtvEnhancer e;
    AtvEnhParams p; p.level = lv; p.outHeight = outH; p.displayAspect = 16.0 / 9.0;
    Buf out; int ow = 0, oh = 0;
    e.process(addNoise(f, 5, rng).data(), w, h, p, out, ow, oh);
    const int N = 6;
    double best = 1e9;
    for (int i = 0; i < N; i++) {
        const Buf n = addNoise(f, 5, rng);
        const auto t0 = std::chrono::steady_clock::now();
        e.process(n.data(), w, h, p, out, ow, oh);
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::printf("time: %s %dx%d -> %dx%d, %s: %.1f ms per picture (best of %d)\n", name, w, h, ow, oh, lv == AtvEnhLevel::Full ? "Full" : "Light", best, N);
}

int main() {
    noiseTest("PAL Light", 768, 576, AtvEnhLevel::Light, 0.6);
    noiseTest("PAL Full", 768, 576, AtvEnhLevel::Full, 0.4);
    noiseTest("NTSC Light", 640, 480, AtvEnhLevel::Light, 0.6);
    noiseTest("NTSC Full", 640, 480, AtvEnhLevel::Full, 0.4);
    geometryTest("PAL 4:3 -> 720", 768, 576, 4.0 / 3.0, 720, AtvEnhLevel::Full);
    geometryTest("PAL 16:9 -> 1080", 768, 576, 16.0 / 9.0, 1080, AtvEnhLevel::Full);
    geometryTest("NTSC 4:3 -> 1080 light", 640, 480, 4.0 / 3.0, 1080, AtvEnhLevel::Light);
    geometryTest("NTSC as received full", 640, 480, 640.0 / 480.0, 0, AtvEnhLevel::Full);
    ghostTest("ghost PAL Light", 768, 576, AtvEnhLevel::Light);
    ghostTest("ghost NTSC Full", 640, 480, AtvEnhLevel::Full);
    combTest(768, 576);
    determinismAndOff();
    timing("PAL", 768, 576, AtvEnhLevel::Light, 720);
    timing("PAL", 768, 576, AtvEnhLevel::Light, 1080);
    timing("PAL", 768, 576, AtvEnhLevel::Full, 720);
    timing("PAL", 768, 576, AtvEnhLevel::Full, 1080);
    std::printf(g_bad ? "FAIL: %d checks\n" : "ok: atv enhancer\n", g_bad);
    return g_bad ? 1 : 0;
}
