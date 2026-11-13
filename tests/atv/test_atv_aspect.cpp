// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Picture aspect ratio of analogue TV (atv_aspect.h, issue #504): the receive choice from the wide screen signalling, the transmit choice of frame and WSS, the
// distortion-free fit of a source into a 4:3 or an anamorphic 16:9 frame, WSS expiry, and the whole chain generator -> receiver with noise, carrier offset and
// sample clock error: the bars of a letterbox or pillarbox in the decoded picture and the WSS the receiver reads back.
#include "dect2/atv/atv_aspect.h"
#include "dect2/atv/atv_vbi.h"
#include "dect2/atv/atv_testkit.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
using namespace dect2::atvkit;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// mean luminance 0..1 of a rectangle (fractions of the picture) of a decoded frame
static double lumaOf(const AtvFrame& f, double x0, double x1, double y0, double y1) {
    double s = 0; long n = 0;
    for (int y = (int)(y0 * f.height); y < (int)(y1 * f.height); y++)
        for (int x = (int)(x0 * f.width); x < (int)(x1 * f.width); x++) {
            const uint8_t* p = &f.rgba[((size_t)y * (size_t)f.width + (size_t)x) * 4];
            s += (p[0] + p[1] + p[2]) / (3.0 * 255.0); n++;
        }
    return n ? s / (double)n : 0;
}

// a grey picture (Y 0.7) of the aspect it claims, as a camera or file would give it
static AtvGenConfig liveCfg(double srcAspect, int aspect, double cnr) {
    AtvGenConfig c;
    c.cnrDb = cnr;
    c.aspect = aspect;
    c.pullVideo = [](double, int w, int h, float* y, float* u, float* v) {
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++) { y[i] = 0.7f; u[i] = 0; v[i] = 0; }
        return true;
    };
    c.pullAspect = [=]() { return srcAspect; };
    return c;
}

int main() {
    // ---- receive: Auto follows the WSS, a letterbox format is still a 4:3 frame, the others are fixed
    CHECK(atvWssIsWide(0b1110) && !atvWssIsWide(0b0001) && !atvWssIsWide(0b1101) && !atvWssIsWide(0b1000) && !atvWssIsWide(-1), "wide WSS bits");
    CHECK(atvWssTextIsWide(atvWssAspectName(0b1110)) && !atvWssTextIsWide(atvWssAspectName(0b1101)) && !atvWssTextIsWide("") , "wide WSS text");
    CHECK(std::fabs(atvDisplayAspect(kAtvAspectAuto, 0b1110) - 16.0 / 9) < 1e-9, "auto + anamorphic 16:9 -> 16:9");
    CHECK(std::fabs(atvDisplayAspect(kAtvAspectAuto, 0b0001) - 4.0 / 3) < 1e-9, "auto + 4:3 -> 4:3");
    CHECK(std::fabs(atvDisplayAspect(kAtvAspectAuto, -1) - 4.0 / 3) < 1e-9, "auto without WSS -> 4:3");
    CHECK(std::fabs(atvDisplayAspect(kAtvAspectAuto, 0b1101) - 4.0 / 3) < 1e-9, "auto + box 16:9 is a 4:3 frame");
    CHECK(std::fabs(atvDisplayAspect(kAtvAspect43, 0b1110) - 4.0 / 3) < 1e-9 && std::fabs(atvDisplayAspect(kAtvAspect169, -1) - 16.0 / 9) < 1e-9, "fixed settings win over the WSS");
    CHECK(std::fabs(atvDisplayAspect(kAtvAspectAuto, std::string(atvWssAspectName(0b1110))) - 16.0 / 9) < 1e-9 && std::fabs(atvDisplayAspect(kAtvAspectAuto, std::string()) - 4.0 / 3) < 1e-9, "text form");
    CHECK(std::fabs(atvDisplayAspect(77, 0b1110) - 16.0 / 9) < 1e-9, "an unknown setting is Auto");

    // ---- transmit: the frame and the WSS
    CHECK(atvTxFrame(kAtvAspectAuto, 16.0 / 9) == kAtvAspect169 && atvTxFrame(kAtvAspectAuto, 4.0 / 3) == kAtvAspect43 && atvTxFrame(kAtvAspectAuto, 0) == kAtvAspect43, "auto frame");
    CHECK(atvTxFrame(kAtvAspectAuto, 1.6) == kAtvAspect169 && atvTxFrame(kAtvAspectAuto, 1.5) == kAtvAspect43 && atvTxFrame(kAtvAspectAuto, 1.0) == kAtvAspect43, "auto frame between the formats");
    CHECK(atvTxFrame(kAtvAspect43, 16.0 / 9) == kAtvAspect43 && atvTxFrame(kAtvAspect169, 4.0 / 3) == kAtvAspect169, "fixed frame");
    CHECK(atvTxWss(kAtvAspect169, kAtvAspect169) == 0b1110 && atvTxWss(kAtvAspectAuto, kAtvAspect169) == 0b1110, "16:9 sends full format 16:9");
    CHECK(atvTxWss(kAtvAspect43, kAtvAspect43) == 0b0001 && atvTxWss(kAtvAspectAuto, kAtvAspect43) == -1, "explicit 4:3 sends full format 4:3, auto + 4:3 none");
    CHECK(atvWssAspectName(atvTxWss(kAtvAspect43, kAtvAspect43)) == std::string("full format 4:3"), "the 4:3 bits are what the receiver names 4:3");
    for (int wss : {0b0001, 0b1110}) { int p = 0; for (int i = 0; i < 4; i++) p += (wss >> i) & 1; CHECK(p & 1, "odd parity of %d", wss); }

    // ---- the fit
    {
        const AtvFit same = atvFitSource(4.0 / 3, 4.0 / 3), narrow = atvFitSource(4.0 / 3, 16.0 / 9), wide = atvFitSource(16.0 / 9, 4.0 / 3), unk = atvFitSource(0, 16.0 / 9);
        CHECK(same.identity() && unk.identity(), "same aspect / unknown: nothing to do");
        CHECK(std::fabs(narrow.x0 - 0.125) < 1e-9 && std::fabs(narrow.x1 - 0.875) < 1e-9 && narrow.y0 == 0 && narrow.y1 == 1, "4:3 in 16:9 takes the middle 75 %% of the width");
        CHECK(std::fabs(wide.y0 - 0.125) < 1e-9 && std::fabs(wide.y1 - 0.875) < 1e-9 && wide.x0 == 0 && wide.x1 == 1, "16:9 in 4:3 takes the middle 75 %% of the height");
    }
    // a disc that is round on the screen stays round: measured in units of the frame height (0.6 across in the source picture)
    for (int dir = 0; dir < 2; dir++) {
        const double srcA = dir ? 16.0 / 9 : 4.0 / 3, frameA = dir ? 4.0 / 3 : 16.0 / 9;
        const int w = 576, h = 288;
        std::vector<float> sy((size_t)w * h), su(sy.size(), 0.f), sv(sy.size(), 0.f), dy(sy.size(), -1.f), du(sy.size(), -1.f), dv(sy.size(), -1.f);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            const double X = (x + 0.5) / w * srcA - srcA / 2, Y = (y + 0.5) / h - 0.5;
            sy[(size_t)y * w + x] = X * X + Y * Y < 0.3 * 0.3 ? 1.f : 0.2f;
        }
        atvFitPlanes(sy.data(), su.data(), sv.data(), w, h, atvFitSource(srcA, frameA), dy.data(), du.data(), dv.data());
        int minx = w, maxx = -1, miny = h, maxy = -1;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (dy[(size_t)y * w + x] > 0.9f) { minx = std::min(minx, x); maxx = std::max(maxx, x); miny = std::min(miny, y); maxy = std::max(maxy, y); }
        const double physW = (maxx - minx + 1) / (double)w * frameA, physH = (maxy - miny + 1) / (double)h;
        // pillarbox (dir 0) keeps the full height: 0.6; letterbox (dir 1) shrinks the whole picture to 75 %% of the frame height: 0.6 * 0.75
        const double want = dir ? 0.6 * 0.75 : 0.6;
        CHECK(std::fabs(physW - want) < 0.02 && std::fabs(physH - want) < 0.02, "dir %d: the disc is %.3f wide and %.3f high (want %.3f)", dir, physW, physH, want);
        // the bars are black (Y 0, U and V 0) and nothing was left unwritten
        bool bars = true, written = true;
        for (size_t i = 0; i < dy.size(); i++) if (dy[i] < -0.5f || du[i] < -0.5f || dv[i] < -0.5f) written = false;
        if (dir == 0) { for (int y = 0; y < h; y++) for (int x = 0; x < (int)(0.12 * w); x++) if (dy[(size_t)y * w + x] != 0.f || du[(size_t)y * w + x] != 0.f) bars = false; }
        else { for (int y = 0; y < (int)(0.12 * h); y++) for (int x = 0; x < w; x++) if (dy[(size_t)y * w + x] != 0.f || dv[(size_t)y * w + x] != 0.f) bars = false; }
        CHECK(bars && written, "dir %d: bars black %d, every sample written %d", dir, (int)bars, (int)written);
    }
    {   // a picture of the frame's own aspect is copied sample for sample
        const int w = 64, h = 32;
        std::vector<float> a((size_t)w * h), b(a.size());
        for (size_t i = 0; i < a.size(); i++) a[i] = (float)(i % 97) / 97.f;
        std::vector<float> z(a.size(), 0.f), o1(a.size()), o2(a.size());
        atvFitPlanes(a.data(), z.data(), z.data(), w, h, AtvFit(), b.data(), o1.data(), o2.data());
        bool same = true; for (size_t i = 0; i < a.size(); i++) if (std::fabs(a[i] - b[i]) > 1e-5f) same = false;
        CHECK(same, "identity fit");
    }

    // ---- WSS that stops is dropped after a few seconds, a repeated one is kept
    {
        AtvVbi v; const auto bits = atvWssElements(0b1110);
        const AtvVbi::Level wss = [&](double u) { const int i = (int)std::floor((u - 11.0) / 0.2); return i >= 0 && i < (int)bits.size() ? (bits[(size_t)i] ? 0.7f : 0.f) : 0.f; };
        const AtvVbi::Level none = [](double) { return 0.f; };
        for (int k = 0; k < 20; k++) { v.line625(wss); for (int j = 0; j < 30; j++) v.line625(none); }
        CHECK(v.status().wssValid && v.status().wssAspect == 0b1110, "WSS held while it keeps coming");
        for (int j = 0; j < 6100; j++) v.line625(none);
        CHECK(!v.status().wssValid && v.status().wssText.empty() && v.status().wssAspect == -1, "WSS dropped after it stopped");
        v.line625(wss);
        CHECK(v.status().wssValid, "and found again");
    }

    // ---- the whole chain: generator -> receiver, with noise, a carrier offset and a sample clock error
    {   // the test card: default setting changes nothing (no WSS, picture to the edges); 16:9 pillarboxes the 4:3 card and sends WSS 1110
        AtvGenConfig c; c.cnrDb = 38; c.cfoHz = 30e3; c.sroPpm = 25;
        const Run r = run(c, 2.5);
        CHECK(r.frame && r.tel.wssText.empty(), "default card: no WSS ('%s')", r.tel.wssText.c_str());
        c.aspect = kAtvAspect169;
        const Run w = run(c, 2.5);
        CHECK(w.frame && w.tel.wssText == "full format 16:9 (anamorphic)", "16:9: WSS '%s'", w.tel.wssText.c_str());
        if (w.frame) {
            const double side = lumaOf(*w.frame, 0.02, 0.09, 0.2, 0.8), mid = lumaOf(*w.frame, 0.3, 0.7, 0.2, 0.8);
            CHECK(side < 0.04 && mid > 0.08, "16:9 card: bars %.3f, middle %.3f", side, mid);
        }
        c.aspect = kAtvAspect43;
        const Run f = run(c, 2.5);
        CHECK(f.tel.wssText == "full format 4:3", "explicit 4:3: WSS '%s'", f.tel.wssText.c_str());
    }
    {   // a 16:9 camera: Auto sends the anamorphic frame with WSS and no bars; 4:3 letterboxes it; a 4:3 camera in 16:9 gets side bars
        const Run a = run(liveCfg(16.0 / 9, kAtvAspectAuto, 38), 2.5);
        CHECK(a.frame && a.tel.wssText == "full format 16:9 (anamorphic)", "16:9 source, auto: WSS '%s'", a.tel.wssText.c_str());
        if (a.frame) CHECK(lumaOf(*a.frame, 0.02, 0.09, 0.3, 0.7) > 0.3 && lumaOf(*a.frame, 0.3, 0.7, 0.03, 0.08) > 0.3, "no bars: %.3f %.3f", lumaOf(*a.frame, 0.02, 0.09, 0.3, 0.7), lumaOf(*a.frame, 0.3, 0.7, 0.03, 0.08));
        const Run b = run(liveCfg(16.0 / 9, kAtvAspect43, 38), 2.5);
        CHECK(b.frame && b.tel.wssText == "full format 4:3", "16:9 source in 4:3: WSS '%s'", b.tel.wssText.c_str());
        if (b.frame) CHECK(lumaOf(*b.frame, 0.3, 0.7, 0.03, 0.08) < 0.05 && lumaOf(*b.frame, 0.3, 0.7, 0.4, 0.6) > 0.3, "letterbox: top %.3f, middle %.3f", lumaOf(*b.frame, 0.3, 0.7, 0.03, 0.08), lumaOf(*b.frame, 0.3, 0.7, 0.4, 0.6));
        const Run d = run(liveCfg(4.0 / 3, kAtvAspect169, 38), 2.5);
        CHECK(d.frame && d.tel.wssText == "full format 16:9 (anamorphic)", "4:3 source in 16:9: WSS '%s'", d.tel.wssText.c_str());
        if (d.frame) CHECK(lumaOf(*d.frame, 0.02, 0.09, 0.3, 0.7) < 0.05 && lumaOf(*d.frame, 0.4, 0.6, 0.3, 0.7) > 0.3, "side bars %.3f, middle %.3f", lumaOf(*d.frame, 0.02, 0.09, 0.3, 0.7), lumaOf(*d.frame, 0.4, 0.6, 0.3, 0.7));
        const Run e = run(liveCfg(4.0 / 3, kAtvAspectAuto, 38), 2.5);
        CHECK(e.frame && e.tel.wssText.empty(), "4:3 source, auto: no WSS ('%s')", e.tel.wssText.c_str());
    }
    {   // a 525-line system has no WSS: the picture is still fitted
        AtvGenConfig c = liveCfg(16.0 / 9, kAtvAspect43, 40); c.sys = kAtvM;
        const Run m = run(c, 2.5);
        CHECK(m.frame && m.tel.wssText.empty(), "525 lines: no WSS");
        if (m.frame) CHECK(lumaOf(*m.frame, 0.3, 0.7, 0.03, 0.08) < 0.06 && lumaOf(*m.frame, 0.3, 0.7, 0.4, 0.6) > 0.3, "525 letterbox: top %.3f, middle %.3f", lumaOf(*m.frame, 0.3, 0.7, 0.03, 0.08), lumaOf(*m.frame, 0.3, 0.7, 0.4, 0.6));
    }
    printf(fails ? "atv_aspect: %d FAILED\n" : "atv_aspect: all passed\n", fails);
    return fails ? 1 : 0;
}
