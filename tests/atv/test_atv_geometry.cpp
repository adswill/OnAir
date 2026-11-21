// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Picture geometry of the analogue TV chain (Discord report: the decoded picture looked squashed vertically, with black bars above and below).
// A picture with a known shape (a disc on a grey ground, round in the 4:3 frame the generator is given) goes through generator -> receiver for every
// line standard. The decoded frame must be 4:3, the disc must come out round (width / height within 3 %), and the grey ground must reach the top and
// the bottom edge of the picture (no bars made by blanking lines being counted as picture).
#include "dect2/atv/atv_aspect.h"
#include "dect2/atv/atv_testkit.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::atvkit;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static double lumaAt(const AtvFrame& f, int x, int y) {
    const uint8_t* p = &f.rgba[((size_t)y * (size_t)f.width + (size_t)x) * 4];
    return (p[0] + p[1] + p[2]) / (3.0 * 255.0);
}

// the disc measured through its centre: the bright run along the middle row (width) and the middle column (height), in samples; stray bright samples elsewhere do not count
static void discSize(const AtvFrame& f, double& w, double& h) {
    auto run = [&](bool row) {
        const int n = row ? f.width : f.height, fixed = row ? f.height / 2 : f.width / 2;
        int best = 0, cur = 0;
        for (int i = 0; i < n; i++) { const double l = row ? lumaAt(f, i, fixed) : lumaAt(f, fixed, i); if (l > 0.6) { if (++cur > best) best = cur; } else cur = 0; }
        return best;
    };
    w = run(true); h = run(false);
}

int main() {
    struct Sys { int sys; const char* name; };
    const Sys systems[] = {{kAtvG, "625 PAL-G"}, {kAtvM, "525 M"}, {kAtvA, "405 A"}};
    for (const Sys& s : systems) {
        AtvGenConfig c;
        c.sys = s.sys; c.colour = kAtvMono; c.cnrDb = 50;
        c.pullVideo = [](double, int w, int h, float* y, float* u, float* v) {
            const double cx = w * 0.5, cy = h * 0.5, r = h * 0.3;
            for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
                const double dx = i + 0.5 - cx, dy = j + 0.5 - cy;
                const size_t o = (size_t)j * w + i;
                y[o] = dx * dx + dy * dy < r * r ? 0.9f : 0.3f; u[o] = 0; v[o] = 0;
            }
            return true;
        };
        const Run r = run(c, 2.5);
        if (!r.frame) { CHECK(false, "%s: no picture", s.name); continue; }
        const AtvFrame& f = *r.frame;
        CHECK(std::fabs((double)f.width / f.height - 4.0 / 3) < 0.02, "%s: frame %d x %d is not 4:3", s.name, f.width, f.height);
        // the ground reaches the first and last rows and columns
        const double top = lumaAt(f, f.width / 8, 2), bot = lumaAt(f, f.width / 8, f.height - 3), left = lumaAt(f, 2, f.height / 8), right = lumaAt(f, f.width - 3, f.height / 8);
        CHECK(top > 0.15 && bot > 0.15 && left > 0.15 && right > 0.15, "%s: bars at the edges: top %.2f bottom %.2f left %.2f right %.2f", s.name, top, bot, left, right);
        double w, h; discSize(f, w, h);
        { printf("%s centre row:", s.name); for (int i = 0; i < 32; i++) printf(" %.2f", lumaAt(f, i * f.width / 32, f.height / 2)); printf("\n"); }
        if (w < 1) { CHECK(false, "%s: disc not found", s.name); continue; }
        const double ratio = w / h;   // a 4:3 frame has square pixels: the disc is round when this is 1
        printf("%s: frame %dx%d, disc %.0f x %.0f, ratio %.3f, height %.3f of the frame (want 0.6)\n", s.name, f.width, f.height, w, h, ratio, h / f.height);
        CHECK(std::fabs(ratio - 1.0) < 0.03, "%s: disc %.0f x %.0f is squashed (ratio %.3f)", s.name, w, h, ratio);
        CHECK(std::fabs(h / f.height - 0.6) < 0.03, "%s: disc height %.3f of the frame, want 0.6", s.name, h / f.height);
    }
    // motion from a picture source (file, camera): no combing in weave. A bar moves 40 samples per field time; the two fields of a frame come from one instant, so
    // the bar edge on even and odd rows of the woven picture must agree to a few samples (it was 40 apart when each field was cut at its own time)
    for (int sysI = 0; sysI < 2; sysI++) for (int ph = 0; ph < 2; ph++) {
        AtvGenConfig c;
        c.sys = sysI ? kAtvM : kAtvG; c.colour = kAtvMono; c.cnrDb = 50; c.startSec = ph * 0.02;
        c.pullVideo = [](double t, int w, int h, float* y, float* u, float* v) {
            const int x0 = 60 + (int)std::fmod(t * 2000.0, 300.0);
            for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) { const size_t o = (size_t)j * w + i; y[o] = i >= x0 && i < x0 + 80 ? 0.9f : 0.2f; u[o] = 0; v[o] = 0; }
            return true;
        };
        const Run r = run(c, 2.5);
        if (!r.frame) { CHECK(false, "comb: no picture"); continue; }
        const AtvFrame& f = *r.frame;
        double sum[2] = {0, 0}; int n[2] = {0, 0};
        for (int y = f.height / 4; y < f.height * 3 / 4; y++) {
            int e = -1; for (int x = 0; x < f.width; x++) if (lumaAt(f, x, y) > 0.55) { e = x; break; }
            if (e >= 0) { sum[y & 1] += e; n[y & 1]++; }
        }
        const double d = n[0] && n[1] ? std::fabs(sum[0] / n[0] - sum[1] / n[1]) : 999;
        printf("comb %s phase %d: even/odd rows differ by %.1f samples\n", sysI ? "525" : "625", ph, d);
        CHECK(d < 6, "%s phase %d: woven rows offset by %.1f samples (combing)", sysI ? "525" : "625", ph, d);
    }
    // a wide source (16:9 and 3:2): a disc round on the screen (physical radius 0.3 of the height) must stay round as the receiver shows it
    // (4:3 frame, or the anamorphic 16:9 frame when WSS says so), whichever aspect setting the generator has; the frame has no bars when it is anamorphic
    struct Wide { double src; int aspect; int sys; const char* name; double shown; double want; };
    const Wide wides[] = {
        {16.0 / 9, kAtvAspectAuto, kAtvG, "16:9 file, auto, 625", 16.0 / 9, 0.3},
        {16.0 / 9, kAtvAspect169, kAtvG, "16:9 file, 16:9, 625", 16.0 / 9, 0.3},
        {16.0 / 9, kAtvAspect43, kAtvG, "16:9 file, 4:3 letterbox, 625", 4.0 / 3, 0.3 * 0.75},
        {16.0 / 9, kAtvAspectAuto, kAtvM, "16:9 file, auto, 525", 4.0 / 3, 0.3 * 0.75},
        {1.5, kAtvAspectAuto, kAtvG, "3:2 file, auto, 625", 4.0 / 3, 0.3 * (4.0 / 3) / 1.5},
    };
    for (const Wide& wd : wides) {
        AtvGenConfig c;
        c.sys = wd.sys; c.colour = kAtvMono; c.cnrDb = 50; c.aspect = wd.aspect;
        const double A = wd.src;
        c.pullAspect = [A]() { return A; };
        c.pullVideo = [A](double, int w, int h, float* y, float* u, float* v) {   // the file stretched to the grid, as VideoIn / SynthMediaVideo(stretch) give it
            for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
                const double px = ((i + 0.5) / w) * A - A * 0.5, py = ((j + 0.5) / h) - 0.5;
                const size_t o = (size_t)j * w + i;
                y[o] = px * px + py * py < 0.3 * 0.3 ? 0.9f : 0.3f; u[o] = 0; v[o] = 0;
            }
            return true;
        };
        const Run r = run(c, 2.5);
        if (!r.frame) { CHECK(false, "%s: no picture", wd.name); continue; }
        const AtvFrame& f = *r.frame;
        double dw, dh; discSize(f, dw, dh);
        if (dw < 1) { CHECK(false, "%s: disc not found", wd.name); continue; }
        const double pw = dw / (double)f.width * wd.shown, ph = dh / (double)f.height;   // physical size on the screen, frame height = 1
        // the receiver shows the frame at 16:9 when the telemetry carries the anamorphic WSS
        const bool wide = r.tel.wssText == "full format 16:9 (anamorphic)";
        CHECK(wide == (wd.shown > 1.5), "%s: WSS '%s'", wd.name, r.tel.wssText.c_str());
        printf("%s: disc %.3f x %.3f (diameter %.3f of the frame height)\n", wd.name, pw, ph, 2 * wd.want);
        CHECK(std::fabs(pw - ph) < 0.03 && std::fabs(ph - 2 * wd.want) < 0.03, "%s: disc %.3f x %.3f on the screen, want %.3f round", wd.name, pw, ph, 2 * wd.want);
    }
    // the window rectangle of the viewer
    {
        using namespace dect2;
        AtvRect a = atvPictureRect(16.0 / 9, 1000, 700, kAtvLetterboxAuto), b = atvPictureRect(4.0 / 3, 1000, 600, kAtvLetterboxAuto), c2 = atvPictureRect(16.0 / 9, 1000, 700, kAtvLetterboxOn),
               d = atvPictureRect(4.0 / 3, 1000, 600, kAtvLetterboxOff), e = atvPictureRect(4.0 / 3, 600, 1000, kAtvLetterboxOn), z = atvPictureRect(0, 800, 600, kAtvLetterboxOn);
        CHECK(a.x == 0 && a.y == 0 && a.w == 1000 && a.h == 700, "auto: 16:9 fills the window");
        CHECK(std::fabs(b.w - 800) < 1e-9 && b.h == 600 && std::fabs(b.x - 100) < 1e-9 && b.y == 0, "auto: 4:3 keeps its shape (side bars)");
        CHECK(std::fabs(c2.w / c2.h - 16.0 / 9) < 1e-9 && c2.w == 1000 && std::fabs(c2.y - (700 - c2.h) / 2) < 1e-9, "on: 16:9 keeps its shape (bars above and below)");
        CHECK(d.w == 1000 && d.h == 600 && d.x == 0, "off: fills the window");
        CHECK(e.w == 600 && std::fabs(e.h - 450) < 1e-9 && std::fabs(e.y - 275) < 1e-9, "tall window");
        CHECK(z.w == 800 && z.h == 600, "unknown aspect: fills");
    }
    printf(fails ? "atv_geometry: %d FAILED\n" : "atv_geometry: all passed\n", fails);
    return fails ? 1 : 0;
}
