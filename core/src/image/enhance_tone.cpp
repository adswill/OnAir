// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Image enhancement, tone: auto levels, gamma, CLAHE, histogram matching, weather chart levels, palettes.
#include "enhance_util.h"

namespace dect2::image {
using namespace detail;

namespace {
constexpr int kBins = 1024;
int bin(float v) { return clampi((int)(v * (kBins - 1) + 0.5f), 0, kBins - 1); }

// v at percentile pct of the histogram
float percentile(const std::vector<double>& hist, double total, double pct) {
    const double target = total * pct / 100.0;
    double acc = 0;
    for (int i = 0; i < (int)hist.size(); i++) { acc += hist[(size_t)i]; if (acc >= target) return (float)i / (float)(hist.size() - 1); }
    return 1.f;
}
}  // namespace

void autoLevels(ImageF& im, double loPct, double hiPct, float maxGain) {
    const std::vector<float> y = lumaPlane(im);
    std::vector<double> hist(kBins, 0);
    for (float v : y) hist[(size_t)bin(v)]++;
    float lo = percentile(hist, (double)y.size(), loPct), hi = percentile(hist, (double)y.size(), hiPct);
    const float minRange = 1.f / std::max(1.f, maxGain);
    if (hi - lo < minRange) {
        const float c = 0.5f * (lo + hi);
        lo = c - 0.5f * minRange; hi = c + 0.5f * minRange;
        if (lo < 0) { hi -= lo; lo = 0; }
        if (hi > 1) { lo -= hi - 1; hi = 1; }
    }
    if (hi - lo < 1e-3f) return;
    const float k = 1.f / (hi - lo);
    for (float& v : im.d) v = clamp01((v - lo) * k);
}

void gamma(ImageF& im, float g) {
    if (g <= 0.01f || std::abs(g - 1.f) < 1e-4f) return;
    float lut[kBins + 1];
    for (int i = 0; i <= kBins; i++) lut[i] = std::pow(i / (float)kBins, 1.f / g);
    for (float& v : im.d) {
        const float t = clamp01(v) * kBins;
        const int i = std::min(kBins - 1, (int)t);
        v = lut[i] + (lut[i + 1] - lut[i]) * (t - i);
    }
}

void clahe(ImageF& im, float clip, int tiles) {
    const int w = im.w, h = im.h;
    if (w < 16 || h < 16) return;
    tiles = std::clamp(tiles, 1, 32);
    const int tx = std::min(tiles, w / 8), ty = std::min(tiles, h / 8);
    const std::vector<float> y = lumaPlane(im);
    constexpr int B = 256;
    std::vector<float> maps((size_t)tx * ty * B);
    ThreadPool::shared().parallelFor((size_t)tx * ty, [&](size_t ti) {
        const int ix = (int)ti % tx, iy = (int)ti / tx;
        const int x0 = ix * w / tx, x1 = (ix + 1) * w / tx, y0 = iy * h / ty, y1 = (iy + 1) * h / ty;
        std::vector<double> hst(B, 0);
        for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) hst[(size_t)clampi((int)(y[(size_t)yy * w + xx] * (B - 1) + 0.5f), 0, B - 1)]++;
        const double n = (double)(x1 - x0) * (y1 - y0);
        const double limit = std::max(1.0, clip * n / B);
        double excess = 0;
        for (double& v : hst) if (v > limit) { excess += v - limit; v = limit; }
        for (double& v : hst) v += excess / B;
        double acc = 0;
        float* m = maps.data() + ti * B;
        for (int i = 0; i < B; i++) { acc += hst[(size_t)i]; m[i] = (float)(acc / n); }
    });
    std::vector<float> ny(y.size());
    forRows(h, [&](int r0, int r1) {
        for (int yy = r0; yy < r1; yy++) {
            // tile centres: (i + 0.5) * size
            const float fy = std::clamp((yy + 0.5f) * ty / h - 0.5f, 0.f, (float)(ty - 1));
            const int a = std::min((int)fy, ty - 1), b = std::min(a + 1, ty - 1);
            const float wy = fy - a;
            for (int xx = 0; xx < w; xx++) {
                const float fx = std::clamp((xx + 0.5f) * tx / w - 0.5f, 0.f, (float)(tx - 1));
                const int c = std::min((int)fx, tx - 1), d = std::min(c + 1, tx - 1);
                const float wx = fx - c;
                const int v = clampi((int)(y[(size_t)yy * w + xx] * (B - 1) + 0.5f), 0, B - 1);
                auto M = [&](int ty_, int tx_) { return maps[((size_t)ty_ * tx + tx_) * B + v]; };
                const float top = M(a, c) * (1 - wx) + M(a, d) * wx, bot = M(b, c) * (1 - wx) + M(b, d) * wx;
                ny[(size_t)yy * w + xx] = top * (1 - wy) + bot * wy;
            }
        }
    });
    applyLuma(im, y, ny);
}

void histMatch(ImageF& im, const ImageF& ref) {
    const std::vector<float> y = lumaPlane(im), r = lumaPlane(ref);
    if (y.empty() || r.empty()) return;
    std::vector<double> hy(kBins, 0), hr(kBins, 0);
    for (float v : y) hy[(size_t)bin(v)]++;
    for (float v : r) hr[(size_t)bin(v)]++;
    std::vector<double> cy(kBins), cr(kBins);
    double a = 0, b = 0;
    for (int i = 0; i < kBins; i++) { a += hy[(size_t)i]; b += hr[(size_t)i]; cy[(size_t)i] = a / (double)y.size(); cr[(size_t)i] = b / (double)r.size(); }
    std::vector<float> lut(kBins);
    int j = 0;
    for (int i = 0; i < kBins; i++) {
        while (j < kBins - 1 && cr[(size_t)j] < cy[(size_t)i]) j++;
        lut[(size_t)i] = (float)j / (kBins - 1);
    }
    std::vector<float> ny(y.size());
    for (size_t i = 0; i < y.size(); i++) ny[i] = lut[(size_t)bin(y[i])];
    applyLuma(im, y, ny);
}

void chartLevels(ImageF& im, float strength) {
    const std::vector<float> y = lumaPlane(im);
    double hist[256] = {};
    for (float v : y) hist[clampi((int)(v * 255.f + 0.5f), 0, 255)]++;
    const double n = (double)y.size();
    double mu = 0;
    for (int i = 0; i < 256; i++) mu += i * hist[i];
    double w0 = 0, s0 = 0, best = -1;
    int t = 128;
    for (int k = 0; k < 255; k++) {
        w0 += hist[k]; s0 += k * hist[k];
        const double w1 = n - w0;
        if (w0 < 1 || w1 < 1) continue;
        const double m0 = s0 / w0, m1 = (mu - s0) / w1;
        const double bc = w0 * w1 * (m0 - m1) * (m0 - m1);
        if (bc > best) { best = bc; t = k; }
    }
    // the ink and paper levels: the mean of each class, trimmed toward its mode (antialiased edges pull the means in)
    double sb = 0, nb = 0, sw = 0, nw = 0;
    for (int k = 0; k <= t; k++) { sb += k * hist[k]; nb += hist[k]; }
    for (int k = t + 1; k < 256; k++) { sw += k * hist[k]; nw += hist[k]; }
    if (nb < 1 || nw < 1) return;
    float black = (float)(sb / nb / 255.0), white = (float)(sw / nw / 255.0);
    int pb = 0, pw = 255;
    for (int k = 0; k <= t; k++) if (hist[k] > hist[pb]) pb = k;
    for (int k = t + 1; k < 256; k++) if (hist[k] > hist[pw]) pw = k;
    black = 0.5f * (black + pb / 255.f); white = 0.5f * (white + pw / 255.f);
    if (white - black < 0.08f) return;
    const float th = ((t + 0.5f) / 255.f - black) / (white - black);
    const float kk = 4.f + 16.f * strength;
    auto sig = [&](float u) { return 1.f / (1.f + std::exp(-(u - th) * kk)); };
    const float s0v = sig(0.f), s1v = sig(1.f);
    std::vector<float> ny(y.size());
    for (size_t i = 0; i < y.size(); i++) {
        const float u = (y[i] - black) / (white - black);
        const float sn = (sig(std::clamp(u, -0.2f, 1.2f)) - s0v) / (s1v - s0v);
        ny[i] = clamp01((1 - strength) * u + strength * sn);
    }
    applyLuma(im, y, ny);
}

ImageF applyPalette(const ImageF& in, Palette p) {
    if (p == Palette::None) return in;
    struct Stop { float v, r, g, b; };
    static const Stop seaLand[] = {{0.00f, 0.02f, 0.05f, 0.20f}, {0.22f, 0.10f, 0.25f, 0.55f}, {0.30f, 0.20f, 0.40f, 0.20f}, {0.45f, 0.45f, 0.50f, 0.25f},
                                   {0.58f, 0.60f, 0.50f, 0.35f}, {0.66f, 0.75f, 0.75f, 0.75f}, {1.00f, 1.00f, 1.00f, 1.00f}};
    // APT infrared: bright = cold. Warm (dark) red to cold (bright) blue and white tops
    static const Stop temp[] = {{0.00f, 0.50f, 0.00f, 0.00f}, {0.20f, 1.00f, 0.20f, 0.00f}, {0.40f, 1.00f, 0.85f, 0.10f}, {0.55f, 0.20f, 0.80f, 0.30f},
                                {0.70f, 0.10f, 0.50f, 1.00f}, {0.85f, 0.30f, 0.10f, 0.70f}, {1.00f, 1.00f, 1.00f, 1.00f}};
    static const Stop rainbow[] = {{0.00f, 0.00f, 0.00f, 0.40f}, {0.20f, 0.00f, 0.30f, 1.00f}, {0.40f, 0.00f, 0.90f, 0.60f}, {0.60f, 0.80f, 1.00f, 0.00f},
                                   {0.80f, 1.00f, 0.40f, 0.00f}, {1.00f, 1.00f, 0.00f, 0.00f}};
    const Stop* s; int ns;
    if (p == Palette::SeaLandCloud) { s = seaLand; ns = 7; } else if (p == Palette::Temperature) { s = temp; ns = 7; } else { s = rainbow; ns = 6; }
    const std::vector<float> y = lumaPlane(in);
    ImageF o(in.w, in.h, 3);
    for (size_t i = 0; i < y.size(); i++) {
        const float v = clamp01(y[i]);
        int k = 0;
        while (k < ns - 2 && v > s[k + 1].v) k++;
        const float t = std::clamp((v - s[k].v) / std::max(1e-6f, s[k + 1].v - s[k].v), 0.f, 1.f);
        o.plane(0)[i] = s[k].r + (s[k + 1].r - s[k].r) * t;
        o.plane(1)[i] = s[k].g + (s[k + 1].g - s[k].g) * t;
        o.plane(2)[i] = s[k].b + (s[k + 1].b - s[k].b) * t;
    }
    return o;
}

}  // namespace dect2::image
