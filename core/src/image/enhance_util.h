// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Internal helpers of the image enhancement (enhance*.cpp): row-band parallel loops on the shared pool, clamps, box and gaussian filters.
#pragma once
#include "dect2/image/enhance.h"
#include "dect2/common/threadpool.h"
#include <algorithm>
#include <cmath>
#include <functional>

namespace dect2::image::detail {

// fn(y0, y1) for bands of rows covering [0, h); bands run on the shared pool, each writes only its own rows
inline void forRows(int h, const std::function<void(int, int)>& fn, int band = 32) {
    if (h <= 0) return;
    const int n = (h + band - 1) / band;
    if (n == 1) { fn(0, h); return; }
    ThreadPool::shared().parallelFor((size_t)n, [&](size_t i) { const int y0 = (int)i * band; fn(y0, std::min(h, y0 + band)); });
}

inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
inline int wrapi(int x, int w) { x %= w; return x < 0 ? x + w : x; }
inline int clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

// 1D gaussian kernel, radius ceil(3 sigma)
inline std::vector<float> gaussKernel(float sigma) {
    const int r = std::max(1, (int)std::ceil(3.f * sigma));
    std::vector<float> k((size_t)(2 * r + 1));
    float s = 0;
    for (int i = -r; i <= r; i++) { k[(size_t)(i + r)] = std::exp(-0.5f * i * i / (sigma * sigma)); s += k[(size_t)(i + r)]; }
    for (float& v : k) v /= s;
    return k;
}

// separable convolution of one plane (edge clamped), out may not alias in
void convolvePlane(const float* in, float* out, int w, int h, const std::vector<float>& k);
// box mean of one plane, radius r (edge clamped)
void boxPlane(const float* in, float* out, int w, int h, int r);
// luma of the picture as one plane (the plane itself for grey)
std::vector<float> lumaPlane(const ImageF& im);
// applies a per-pixel luma remap: grey -> new value; colour -> R, G, B scaled by newY / oldY (hue kept), clamped
void applyLuma(ImageF& im, const std::vector<float>& oldY, const std::vector<float>& newY);

}  // namespace dect2::image::detail
