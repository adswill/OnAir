// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Picture aspect ratio of analogue television: see atv_aspect.h.
#include "dect2/atv/atv_aspect.h"
#include "dect2/atv/atv_vbi.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

bool atvWssIsWide(int bits) { return bits >= 0 && (bits & 15) == kAtvWssFull169; }
bool atvWssTextIsWide(const std::string& t) { return !t.empty() && t == atvWssAspectName(kAtvWssFull169); }

double atvDisplayAspect(int mode, int wss) {
    if (mode == kAtvAspect43) return kAtvAspectValue43;
    if (mode == kAtvAspect169) return kAtvAspectValue169;
    return atvWssIsWide(wss) ? kAtvAspectValue169 : kAtvAspectValue43;
}
double atvDisplayAspect(int mode, const std::string& wssText) {
    if (mode == kAtvAspect43) return kAtvAspectValue43;
    if (mode == kAtvAspect169) return kAtvAspectValue169;
    return atvWssTextIsWide(wssText) ? kAtvAspectValue169 : kAtvAspectValue43;
}

int atvTxFrame(int mode, double src) {
    if (mode == kAtvAspect43) return kAtvAspect43;
    if (mode == kAtvAspect169) return kAtvAspect169;
    return src >= 1.55 ? kAtvAspect169 : kAtvAspect43;   // 1.55 lies between 4:3 (1.33) and 16:9 (1.78)
}
int atvTxWss(int mode, int frame) {
    if (frame == kAtvAspect169) return kAtvWssFull169;
    return mode == kAtvAspect43 ? kAtvWssFull43 : -1;
}

AtvFit atvFitSource(double src, double frame) {
    AtvFit f;
    if (!(src > 0.1) || !(frame > 0.1) || std::fabs(src - frame) < 1e-3 * frame) return f;
    if (src > frame) { const double h = frame / src; f.y0 = 0.5 - 0.5 * h; f.y1 = 0.5 + 0.5 * h; }   // wider than the frame: bars above and below
    else { const double w = src / frame; f.x0 = 0.5 - 0.5 * w; f.x1 = 0.5 + 0.5 * w; }               // narrower: bars at the sides
    return f;
}

void atvFitPlanes(const float* sy, const float* su, const float* sv, int w, int h, const AtvFit& fit, float* dy, float* du, float* dv) {
    if (w <= 0 || h <= 0) return;
    const double bw = fit.x1 - fit.x0, bh = fit.y1 - fit.y0;
    for (int y = 0; y < h; y++) {
        const double vy = ((y + 0.5) / h - fit.y0) / bh;            // 0 .. 1 over the picture inside the rectangle
        const bool rowIn = bh > 0 && vy >= 0 && vy < 1;
        const double fy = vy * h - 0.5;
        const int y0 = std::max(0, std::min(h - 1, (int)std::floor(fy))), y1 = std::min(h - 1, y0 + 1);
        const float ty = (float)std::max(0.0, std::min(1.0, fy - y0));
        for (int x = 0; x < w; x++) {
            const size_t o = (size_t)y * (size_t)w + (size_t)x;
            const double vx = ((x + 0.5) / w - fit.x0) / bw;
            if (!rowIn || !(bw > 0) || vx < 0 || vx >= 1) { dy[o] = 0; du[o] = 0; dv[o] = 0; continue; }
            const double fx = vx * w - 0.5;
            const int x0 = std::max(0, std::min(w - 1, (int)std::floor(fx))), x1 = std::min(w - 1, x0 + 1);
            const float tx = (float)std::max(0.0, std::min(1.0, fx - x0));
            auto at = [&](const float* p) {
                const float a = p[(size_t)y0 * (size_t)w + (size_t)x0], b = p[(size_t)y0 * (size_t)w + (size_t)x1];
                const float c = p[(size_t)y1 * (size_t)w + (size_t)x0], d = p[(size_t)y1 * (size_t)w + (size_t)x1];
                const float top = a + (b - a) * tx, bot = c + (d - c) * tx;
                return top + (bot - top) * ty;
            };
            dy[o] = at(sy); du[o] = at(su); dv[o] = at(sv);
        }
    }
}

} // namespace dect2
