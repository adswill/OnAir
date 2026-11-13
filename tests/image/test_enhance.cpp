// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Image enhancement (core/include/dect2/image/enhance.h): synthetic pictures with known degradations (slant, phase, missing and
// corrupted lines, jitter, impulses, gaussian noise, stripes, chroma lag, interlace, repeated noisy copies) and measurable improvement
// against the clean picture: PSNR, SSIM, slant error in px. Prints the numbers used in docs/IMAGE-ENHANCE.md.
#include "dect2/image/enhance.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
using namespace dect2::image;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static float sat(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// a weather chart: white paper, a black sync bar at the line start, antialiased isobars, a grid, text-like blocks
static ImageF chart(int w, int h) {
    ImageF im(w, h, 1, 1.f);
    const int bar = w / 20;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float v = 1.f;
            if (x < bar) v = 0.f;
            else {
                for (int k = 0; k < 6; k++) {   // isobars
                    const float cy = h * (k + 0.5f) / 6 + 25.f * std::sin(x * 0.012f + k);
                    const float d = std::abs(y - cy);
                    v = std::min(v, sat(d - 0.6f));
                }
                if (x % 150 == 100 || y % 150 == 75) v = std::min(v, 0.35f);   // grid
                if ((x / 6) % 3 != 0 && (y / 8) % 9 == 4 && x > w / 2 && x < w / 2 + 180 && (y / 8) % 2 == 0) v = 0.f;   // "text"
            }
            im.at(0, x, y) = v;
        }
    return im;
}

// a colour test photo: gradients, discs, bars with vertical edges, texture
static ImageF photo(int w, int h) {
    ImageF im(w, h, 3);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float r = 0.2f + 0.6f * x / w, g = 0.3f + 0.5f * y / h, b = 0.5f + 0.2f * std::sin(x * 0.05f) * std::cos(y * 0.04f);
            const float dx = x - w * 0.35f, dy = y - h * 0.45f;
            if (dx * dx + dy * dy < (h * 0.2f) * (h * 0.2f)) { r = 0.9f; g = 0.2f; b = 0.15f; }
            if ((x / 24) % 2 == 0 && y > h * 0.7f) { r *= 0.3f; g *= 0.3f; b *= 0.3f; }
            if (x > w * 0.65f && x < w * 0.8f && y > h * 0.15f && y < h * 0.55f) { r = 0.1f; g = 0.6f; b = 0.9f; }
            im.at(0, x, y) = sat(r); im.at(1, x, y) = sat(g); im.at(2, x, y) = sat(b);
        }
    return im;
}

static ImageF addNoise(ImageF im, float sigma, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> n(0.f, sigma);
    for (float& v : im.d) v = sat(v + n(rng));
    return im;
}

static ImageF slantIt(const ImageF& im, double pxPerLine, int phase) {   // line y moved right by pxPerLine * y + phase (circular)
    return deslant(im, -pxPerLine, -phase, true);
}

int main() {
    // ---------------------------------------------------------------- fax: slant + phase + bad lines + impulses, Chart preset
    {
        const int W = 1200, H = 800;
        const ImageF clean = chart(W, H);
        const double trueSlant = 0.15;
        ImageF bad = slantIt(clean, trueSlant, 300);
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> u(0.f, 1.f);
        const int noisy[] = {100, 101, 333, 512, 700}, missing[] = {200, 201, 202};
        for (int y : noisy) for (int x = 0; x < W; x++) bad.at(0, x, y) = u(rng);
        for (int y : missing) for (int x = 0; x < W; x++) bad.at(0, x, y) = 0.f;
        for (int i = 0; i < W * H / 200; i++) { const int x = (int)(u(rng) * W) % W, y = (int)(u(rng) * H) % H; bad.at(0, x, y) = u(rng) < 0.5f ? 0.f : 1.f; }
        const SlantFit f = estimateSlant(bad);
        printf("fax slant: true %.4f est %.4f px/line (error %.4f px/line = %.2f px over %d lines), rms %.3f, %d pairs\n", trueSlant, f.pxPerLine,
               f.pxPerLine - trueSlant, (f.pxPerLine - trueSlant) * H, H, f.rms, f.used);
        CHECK(std::abs(f.pxPerLine - trueSlant) < 0.005, "slant %.4f", f.pxPerLine);
        Params p = presetParams(Preset::Chart, Kind::Fax);
        Report r;
        const ImageF out = enhance(bad, p, &r);
        // the phase found should put the sync bar back at column 0: compare with the clean chart directly
        const double pb = psnr(bad, clean), pa = psnr(out, clean), sb = ssim(bad, clean), sa = ssim(out, clean);
        printf("fax chart: PSNR %.2f -> %.2f dB, SSIM %.3f -> %.3f, phase %d, repaired %d lines, %d impulses, %.0f ms\n", pb, pa, sb, sa, r.phase, r.repaired, r.impulses, r.ms);
        CHECK(pa > pb + 8, "chart psnr %.2f -> %.2f", pb, pa);
        CHECK(sa > sb + 0.2, "chart ssim");
        CHECK(r.repaired >= 8 && r.repaired <= 12, "repaired %d", r.repaired);
        // a residual slant check: the sync bar edge position on the first and last line
        auto edge = [&](int y) { int x = 0; while (x < W && out.at(0, x, y) < 0.5f) x++; return x; };
        printf("fax sync edge after: line 10 at %d, line %d at %d (clean %d)\n", edge(10), H - 10, edge(H - 10), W / 20);
        CHECK(std::abs(edge(10) - W / 20) <= 2 && std::abs(edge(H - 10) - W / 20) <= 2, "edge %d %d", edge(10), edge(H - 10));
        // Chart never blurs: a clean chart through the Chart preset keeps its PSNR high
        Params pc = p; pc.autoSlant = pc.autoPhase = false;
        const double keep = psnr(enhance(clean, pc), clean);
        printf("fax chart, clean input through Chart: PSNR %.2f dB\n", keep);
        CHECK(keep > 24, "chart preset altered a clean chart: %.2f", keep);
        // phase alone
        CHECK(findPhase(rotateColumns(clean, 777)) == (W - 777) % W || findPhase(rotateColumns(clean, 777)) == W - 777, "phase");
        CHECK(findPhase(rotateColumns(clean, 40)) == W - 40, "phase small %d", findPhase(rotateColumns(clean, 40)));
        // jitter: register lines
        ImageF jit = clean;
        std::uniform_int_distribution<int> j(-3, 3);
        for (int y = 0; y < H; y++) { const int s = j(rng); ImageF row(W, 1, 1); for (int x = 0; x < W; x++) row.at(0, x, 0) = clean.at(0, ((x - s) % W + W) % W, y); for (int x = 0; x < W; x++) jit.at(0, x, y) = row.at(0, x, 0); }
        ImageF reg = jit;
        registerLines(reg);
        printf("fax jitter +-3 px: PSNR %.2f -> %.2f dB\n", psnr(jit, clean), psnr(reg, clean));
        CHECK(psnr(reg, clean) > psnr(jit, clean) + 3, "register");
        // temporal average of 3 noisy, shifted copies
        std::vector<ImageF> copies;
        copies.push_back(addNoise(clean, 0.15f, 1));
        copies.push_back(addNoise(deslant(clean, 0, -7, true), 0.15f, 2));
        {
            ImageF c = addNoise(clean, 0.15f, 3), s(W, H, 1, 1.f);
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) s.at(0, x, y) = y >= 3 ? c.at(0, x, y - 3) : 1.f;   // 3 lines later
            copies.push_back(s);
        }
        std::vector<int> dx, dy;
        const ImageF avg = temporalAverage(copies, 16, &dx, &dy);
        printf("fax repeats x3: shifts (%d,%d) (%d,%d), PSNR %.2f -> %.2f dB\n", dx[1], dy[1], dx[2], dy[2], psnr(copies[0], clean), psnr(avg, clean));
        CHECK(dx.size() == 3 && dx[1] == 7 && dy[1] == 0 && dx[2] == 0 && dy[2] == 3, "temporal shifts");
        CHECK(psnr(avg, clean) > psnr(copies[0], clean) + 3, "temporal psnr");
    }
    // ---------------------------------------------------------------- SSTV colour: noise, slant, chroma lag, Colour SSTV preset
    {
        const int W = 320, H = 256;
        const ImageF clean = photo(W, H);
        // slant estimation on a photo with vertical edges
        const double ts = 0.06;
        const ImageF sl = slantIt(clean, ts, 0);
        const SlantFit f = estimateSlant(luma(sl));
        printf("sstv slant: true %.4f est %.4f px/line (error %.2f px over %d lines)\n", ts, f.pxPerLine, (f.pxPerLine - ts) * H, H);
        CHECK(std::abs(f.pxPerLine - ts) * H < 2.0, "sstv slant %.4f", f.pxPerLine);
        // chroma lag of 3 px
        ImageF lag = clean;
        toYCbCr(lag);
        for (int c = 1; c < 3; c++) for (int y = 0; y < H; y++) for (int x = W - 1; x >= 0; x--) lag.at(c, x, y) = lag.at(c, std::max(0, x - 3), y);
        fromYCbCr(lag);
        ImageF fixd = lag;
        const double cs = chromaFix(fixd, 0.8f);
        printf("sstv chroma lag 3 px: found %.2f px, PSNR %.2f -> %.2f dB\n", cs, psnr(lag, clean), psnr(fixd, clean));
        CHECK(std::abs(cs - 3) < 0.6, "chroma shift %.2f", cs);
        CHECK(psnr(fixd, clean) > psnr(lag, clean) + 2, "chroma psnr");
        // noise: bilateral and NLM
        const ImageF noisy = addNoise(clean, 0.07f, 11);
        const ImageF bl = denoise(noisy, 50, false), nl = denoise(noisy, 50, true);
        printf("sstv noise 0.07: PSNR %.2f, bilateral %.2f, NLM %.2f dB; SSIM %.3f, %.3f, %.3f\n", psnr(noisy, clean), psnr(bl, clean), psnr(nl, clean),
               ssim(noisy, clean), ssim(bl, clean), ssim(nl, clean));
        CHECK(psnr(bl, clean) > psnr(noisy, clean) + 3 && psnr(nl, clean) > psnr(noisy, clean) + 3, "denoise psnr");
        CHECK(ssim(bl, clean) > ssim(noisy, clean) + 0.1 && ssim(nl, clean) > ssim(noisy, clean) + 0.1, "denoise ssim");
        // the preset on noise + slant + 4 missing lines
        ImageF all = addNoise(slantIt(clean, ts, 0), 0.07f, 12);
        for (int y : {60, 61, 150, 200}) for (int c = 0; c < 3; c++) for (int x = 0; x < W; x++) all.at(c, x, y) = 0;
        Params p = presetParams(Preset::ColourSstv, Kind::Sstv);
        p.levels = false; p.greyWorld = false;   // the reference is not a stretched picture
        Report r;
        const ImageF out = enhance(all, p, &r);
        printf("sstv all: PSNR %.2f -> %.2f dB, SSIM %.3f -> %.3f (slant %.4f, %d lines repaired, %.0f ms)\n", psnr(all, clean), psnr(out, clean), ssim(all, clean), ssim(out, clean), r.slantPx, r.repaired, r.ms);
        CHECK(psnr(out, clean) > psnr(all, clean) + 4, "sstv preset psnr");
        // grey world: a cast
        ImageF cast = clean;
        for (float* q = cast.plane(2); q < cast.plane(2) + W * H; q++) *q *= 0.6f;
        greyWorld(cast);
        double m[3] = {};
        for (int c = 0; c < 3; c++) { for (int i = 0; i < W * H; i++) m[c] += cast.plane(c)[i]; m[c] /= W * H; }
        CHECK(std::abs(m[2] - m[1]) < 0.08, "grey world %.3f %.3f %.3f", m[0], m[1], m[2]);
        // deinterlace: odd field from a different moment
        ImageF il = luma(clean);
        const ImageF ilc = il;
        for (int y = 1; y < H; y += 2) for (int x = 0; x < W; x++) il.at(0, x, y) = ilc.at(0, std::min(W - 1, x + 6), y);
        ImageF di = il;
        const int nd = deinterlace(di);
        printf("deinterlace: %d px, PSNR %.2f -> %.2f dB\n", nd, psnr(il, ilc), psnr(di, ilc));
        CHECK(psnr(di, ilc) > psnr(il, ilc) + 1, "deinterlace");
    }
    // ---------------------------------------------------------------- satellite: stripes, impulses, levels, CLAHE, palette
    {
        const int W = 900, H = 600;
        ImageF clean(W, H, 1);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
            clean.at(0, x, y) = sat(0.45f + 0.25f * std::sin(x * 0.01f + y * 0.007f) + 0.15f * std::cos(x * 0.031f) * std::sin(y * 0.023f));
        ImageF st = clean;
        std::mt19937 rng(5);
        std::uniform_real_distribution<float> g(0.8f, 1.2f), o(-0.06f, 0.06f);
        for (int y = 0; y < H; y++) { const float gg = g(rng), oo = o(rng); for (int x = 0; x < W; x++) st.at(0, x, y) = sat((st.at(0, x, y) - 0.5f) * gg + 0.5f + oo); }
        ImageF ds = st;
        destripe(ds);
        printf("apt stripes: PSNR %.2f -> %.2f dB, SSIM %.3f -> %.3f\n", psnr(st, clean), psnr(ds, clean), ssim(st, clean), ssim(ds, clean));
        CHECK(psnr(ds, clean) > psnr(st, clean) + 6, "destripe");
        // impulses: few false hits on the clean picture, a big gain on 2 % salt and pepper
        ImageF c2 = clean;
        const int fp = switchingMedian(c2);
        ImageF sp = clean;
        std::uniform_real_distribution<float> u(0.f, 1.f);
        for (int i = 0; i < W * H / 50; i++) sp.at(0, (int)(u(rng) * W) % W, (int)(u(rng) * H) % H) = u(rng) < 0.5f ? 0.f : 1.f;
        ImageF sm = sp;
        const int hits = switchingMedian(sm);
        printf("impulses 2%%: %d replaced (false on clean: %d), PSNR %.2f -> %.2f dB\n", hits, fp, psnr(sp, clean), psnr(sm, clean));
        CHECK(fp == 0 && psnr(sm, clean) > psnr(sp, clean) + 12, "switching median");
        // low contrast: auto levels widens; protected: a dark flat picture is not blown up
        ImageF lc = clean;
        for (float& v : lc.d) v = 0.4f + 0.2f * v;
        ImageF al = lc;
        autoLevels(al);
        float mn = 1, mx = 0, imn = 1, imx = 0;
        for (float v : al.d) { mn = std::min(mn, v); mx = std::max(mx, v); }
        for (float v : lc.d) { imn = std::min(imn, v); imx = std::max(imx, v); }
        CHECK(mx - mn > 3.9f * (imx - imn), "levels %.2f %.2f from %.2f %.2f", mn, mx, imn, imx);   // a narrow range gets the gain limit, 4
        auto sd = [](const ImageF& a) { double m = 0, q = 0; for (float v : a.d) { m += v; q += v * v; } m /= a.d.size(); return std::sqrt(q / a.d.size() - m * m); };
        ImageF dark = addNoise(ImageF(200, 200, 1, 0.05f), 0.01f, 3);
        const ImageF dark0 = dark;
        autoLevels(dark);
        CHECK(sd(dark) <= 4.05 * sd(dark0), "protected levels stretched noise: %.4f -> %.4f", sd(dark0), sd(dark));
        ImageF ch = lc;
        clahe(ch, 3.f);
        printf("clahe: spread %.3f -> %.3f\n", sd(lc), sd(ch));
        CHECK(sd(ch) > 1.6 * sd(lc), "clahe");
        ImageF ref = clean, hm = lc;
        histMatch(hm, ref);
        CHECK(psnr(hm, clean) > psnr(lc, clean) + 10, "hist match %.2f %.2f", psnr(lc, clean), psnr(hm, clean));
        const ImageF pal = applyPalette(clean, Palette::Temperature);
        CHECK(pal.ch == 3 && pal.w == W, "palette");
    }
    // ---------------------------------------------------------------- upscale without ringing, unsharp
    {
        const int W = 200, H = 160;
        const ImageF big = luma(photo(W * 2, H * 2));
        const ImageF small = resize(big, W, H);
        const ImageF lz = lanczos2x(small, 0.f), bl = resize(small, W * 2, H * 2);
        printf("x2 upscale: bilinear PSNR %.2f, Lanczos %.2f dB\n", psnr(bl, big), psnr(lz, big));
        CHECK(psnr(lz, big) > psnr(bl, big), "lanczos beats bilinear");
        ImageF step(64, 8, 1, 0.f);
        for (int y = 0; y < 8; y++) for (int x = 32; x < 64; x++) step.at(0, x, y) = 0.8f;
        const ImageF su = lanczos2x(step, 0.f);
        float mn = 1, mx = 0;
        for (float v : su.d) { mn = std::min(mn, v); mx = std::max(mx, v); }
        CHECK(mn >= 0.f && mx <= 0.8f + 1e-5f, "ringing %.4f %.4f", mn, mx);
        const ImageF blur = [&] { ImageF b = big; gaussian(b, 1.0f); return b; }();
        ImageF us = blur;
        unsharp(us, 1.0f, 0.8f);
        CHECK(psnr(us, big) > psnr(blur, big), "unsharp %.2f %.2f", psnr(blur, big), psnr(us, big));
    }
    // ---------------------------------------------------------------- line repair alone, determinism, params text, speed
    {
        const int W = 640, H = 480;
        const ImageF clean = luma(photo(W, H));
        ImageF dmg = clean;
        std::mt19937 rng(9);
        std::uniform_real_distribution<float> u(0.f, 1.f);
        const std::vector<int> lines = {10, 11, 12, 100, 240, 241, 400, 470};
        for (size_t i = 0; i < lines.size(); i++) for (int x = 0; x < W; x++) dmg.at(0, x, lines[i]) = i % 2 ? u(rng) : 1.f;
        std::vector<int> fixed;
        ImageF rp = dmg;
        repairLines(rp, 0.35, &fixed);
        printf("missing/corrupt lines (%zu): found %zu, PSNR %.2f -> %.2f dB\n", lines.size(), fixed.size(), psnr(dmg, clean), psnr(rp, clean));
        CHECK(fixed == lines, "repaired set");
        CHECK(psnr(rp, clean) > psnr(dmg, clean) + 10, "repair psnr");
        ImageF cl = clean;
        CHECK(repairLines(cl) == 0, "false repair on a clean picture");

        Params p = presetParams(Preset::Photo, Kind::Sstv);
        const ImageF a = enhance(dmg, p), b = enhance(dmg, p);
        CHECK(a.d == b.d, "not deterministic");
        {   // the same for any thread count: compare this line between DECT2_THREADS=0 and the default
            Params all = presetParams(Preset::ColourSstv, Kind::Sstv); all.denoise = 30; all.nlm = true; all.clahe = 2; all.registerRows = true;
            const ImageF c = enhance(photo(W, H), all);
            uint64_t hsh = 1469598103934665603ull;
            for (float v : c.d) { uint32_t u; memcpy(&u, &v, 4); hsh = (hsh ^ u) * 1099511628211ull; }
            printf("checksum %016llx\n", (unsigned long long)hsh);
        }
        Params q = presetParams(Preset::Satellite, Kind::Apt);
        q.palette = Palette::SeaLandCloud; q.ioc = 288; q.slant = -0.25f;
        const Params q2 = paramsFromString(paramsToString(q), Params());
        CHECK(paramsToString(q2) == paramsToString(q), "params text");
        CHECK(kindForSource("Weather fax") == Kind::Fax && kindForSource("SSTV") == Kind::Sstv && kindForSource("NOAA APT") == Kind::Apt &&
              kindForSource("Meteor-M LRPT") == Kind::Satellite && kindForSource("DRM slide show") == Kind::Slide, "kinds");
        CHECK(iocWidth(clean, 576).w == 1810, "ioc");

        // speed: 1000 x 1000 grey and colour through the presets (the live preview path and the full path)
        const ImageF big = luma(photo(1000, 1000));
        const ImageF bigc = photo(1000, 1000);
        struct Case { const char* name; Preset pr; Kind k; bool colour; } cases[] = {
            {"Chart (fax)", Preset::Chart, Kind::Fax, false}, {"Photo", Preset::Photo, Kind::Slide, false},
            {"Satellite", Preset::Satellite, Kind::Apt, false}, {"Colour SSTV", Preset::ColourSstv, Kind::Sstv, true}};
        for (const Case& c : cases) {
            const Params pp = presetParams(c.pr, c.k);
            double best = 1e9, bestPrev = 1e9;
            for (int i = 0; i < 3; i++) {
                Report r;
                enhance(c.colour ? bigc : big, pp, &r, false); best = std::min(best, r.ms);
                enhance(c.colour ? bigc : big, pp, &r, true); bestPrev = std::min(bestPrev, r.ms);
            }
            printf("speed 1000x1000 %-12s full %.0f ms, preview %.0f ms\n", c.name, best, bestPrev);
            CHECK(bestPrev < 1500, "too slow");
        }
        Params nl; nl.denoise = 40; nl.nlm = true;
        Report r;
        enhance(big, nl, &r);
        printf("speed 1000x1000 NLM 40      %.0f ms\n", r.ms);
        Params up; up.upscale = true; up.sharpen = 0.4f;
        enhance(big, up, &r);
        printf("speed 1000x1000 Lanczos x2  %.0f ms\n", r.ms);
        // split view
        std::vector<uint8_t> A(16 * 4 * 4, 10), B(16 * 4 * 4, 200), O(16 * 4 * 4);
        splitView(A.data(), B.data(), 16, 4, 0.5f, O.data());
        CHECK(O[0] == 10 && O[15 * 4] == 200, "split");
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("enhance OK\n");
    return 0;
}
