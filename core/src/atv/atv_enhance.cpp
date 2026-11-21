// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Analog TV video enhancer (atv_enhance.h). Planes are 8 bit Y, Cb, Cr (BT.601 full range, the same arithmetic both ways).
#include "dect2/atv/atv_enhance.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace dect2 {
namespace {

inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
inline uint8_t clamp8f(float v) { return (uint8_t)(v < 0.f ? 0 : v > 255.f ? 255 : (int)(v + 0.5f)); }

void toYcc(const uint8_t* rgba, size_t n, uint8_t* Y, uint8_t* Cb, uint8_t* Cr) {
    for (size_t i = 0; i < n; i++) {
        const int r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
        Y[i] = clamp8((77 * r + 150 * g + 29 * b + 128) >> 8);
        Cb[i] = clamp8(((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128);
        Cr[i] = clamp8(((128 * r - 107 * g - 21 * b + 128) >> 8) + 128);
    }
}

void toRgba(const uint8_t* Y, const uint8_t* Cb, const uint8_t* Cr, size_t n, uint8_t* rgba) {
    for (size_t i = 0; i < n; i++) {
        const int y = Y[i], cb = Cb[i] - 128, cr = Cr[i] - 128;
        rgba[i * 4] = clamp8(y + ((359 * cr + 128) >> 8));
        rgba[i * 4 + 1] = clamp8(y - ((88 * cb + 183 * cr + 128) >> 8));
        rgba[i * 4 + 2] = clamp8(y + ((454 * cb + 128) >> 8));
        rgba[i * 4 + 3] = 255;
    }
}

// range weights for an absolute difference 0..255: exp(-d^2 / (2 s^2))
void rangeLut(float* lut, float s) {
    const float k = -0.5f / (s * s);
    for (int d = 0; d < 256; d++) lut[d] = std::exp(k * (float)d * (float)d);
}

// ---- scaling: one axis of taps (start index, weights), clamped at the edges, normalised
struct Taps {
    int n = 0;                      // taps per output sample
    std::vector<int> start;         // per output sample
    std::vector<float> w;           // out * n
};

float lanczos(float x, float a) {
    x = std::fabs(x);
    if (x < 1e-6f) return 1.f;
    if (x >= a) return 0.f;
    const float px = 3.14159265358979f * x;
    return a * std::sin(px) * std::sin(px / a) / (px * px);
}

Taps makeTaps(int in, int out, bool lanczos3) {
    Taps t;
    const float scale = (float)in / (float)out;
    const float support = lanczos3 ? 3.f : 1.f;
    const float fs = std::max(1.f, scale);   // widen the kernel when shrinking
    t.n = (int)std::ceil(2.f * support * fs) + 1;
    t.start.resize(out);
    t.w.assign((size_t)out * t.n, 0.f);
    for (int o = 0; o < out; o++) {
        const float c = ((float)o + 0.5f) * scale - 0.5f;   // source position of the output sample centre
        const int s = (int)std::floor(c - support * fs) + 1;
        t.start[o] = s;
        float sum = 0.f;
        float* w = &t.w[(size_t)o * t.n];
        for (int k = 0; k < t.n; k++) {
            const float d = ((float)(s + k) - c) / fs;
            const float v = lanczos3 ? lanczos(d, 3.f) : std::max(0.f, 1.f - std::fabs(d));
            w[k] = v; sum += v;
        }
        if (sum != 0.f) for (int k = 0; k < t.n; k++) w[k] /= sum;
    }
    return t;
}

void scalePlane(const uint8_t* src, int w, int h, uint8_t* dst, int ow, int oh, bool lanczos3, std::vector<float>& tmp) {
    if (ow == w && oh == h) { std::memcpy(dst, src, (size_t)w * h); return; }
    const Taps tx = makeTaps(w, ow, lanczos3), ty = makeTaps(h, oh, lanczos3);
    tmp.resize((size_t)ow * h);
    for (int y = 0; y < h; y++) {
        const uint8_t* s = src + (size_t)y * w;
        float* d = &tmp[(size_t)y * ow];
        for (int x = 0; x < ow; x++) {
            const float* wt = &tx.w[(size_t)x * tx.n];
            const int s0 = tx.start[x];
            float acc = 0.f;
            if (s0 >= 0 && s0 + tx.n <= w) for (int k = 0; k < tx.n; k++) acc += wt[k] * (float)s[s0 + k];
            else for (int k = 0; k < tx.n; k++) acc += wt[k] * (float)s[std::min(w - 1, std::max(0, s0 + k))];
            d[x] = acc;
        }
    }
    for (int y = 0; y < oh; y++) {
        const float* wt = &ty.w[(size_t)y * ty.n];
        const int s0 = ty.start[y];
        uint8_t* d = dst + (size_t)y * ow;
        for (int x = 0; x < ow; x++) {
            float a = 0.f;
            for (int k = 0; k < ty.n; k++) a += wt[k] * tmp[(size_t)std::min(h - 1, std::max(0, s0 + k)) * ow + x];
            d[x] = clamp8f(a);
        }
    }
}

} // namespace

void AtvEnhancer::reset() { have_ = false; pw_ = ph_ = 0; sigma_ = 3.f; pY_.clear(); pCb_.clear(); pCr_.clear(); }

void AtvEnhancer::outputSize(int w, int h, const AtvEnhParams& p, int& ow, int& oh) {
    if (p.outHeight <= 0) { ow = w; oh = h; return; }
    const double asp = p.displayAspect > 0 ? p.displayAspect : (h > 0 ? (double)w / h : 4.0 / 3.0);
    oh = p.outHeight;
    ow = (int)std::lround(oh * asp / 2.0) * 2;
    if (ow < 2) ow = 2;
}

bool AtvEnhancer::process(const uint8_t* rgba, int w, int h, const AtvEnhParams& p, std::vector<uint8_t>& out, int& ow, int& oh) {
    if (!rgba || w < 8 || h < 8 || p.level == AtvEnhLevel::Off) return false;
    const bool full = p.level == AtvEnhLevel::Full;
    const size_t n = (size_t)w * h;
    std::vector<uint8_t> Y(n), Cb(n), Cr(n);
    toYcc(rgba, n, Y.data(), Cb.data(), Cr.data());
    if (!have_ || pw_ != w || ph_ != h) { have_ = false; pw_ = w; ph_ = h; sigma_ = 3.f; }

    // ---- noise level and motion against the previous (filtered) picture
    std::vector<uint8_t> motion;
    float T = 14.f;
    if (have_) {
        double acc = 0; int cnt = 0;
        for (int y = 1; y < h; y += 4) for (int x = 1; x < w; x += 4) {
            const int d = std::abs((int)Y[(size_t)y * w + x] - (int)pY_[(size_t)y * w + x]);
            if (d < 24) { acc += d; cnt++; }
        }
        if (cnt > 64) sigma_ = 0.8f * sigma_ + 0.2f * std::max(0.8f, (float)(acc / cnt) / 1.128f);
        T = std::min(48.f, std::max(8.f, 3.2f * sigma_ + 4.f));
        motion.assign(n, 0);
        for (int y = 0; y < h; y++) {
            const size_t r = (size_t)y * w;
            for (int x = 0; x < w; x++) {
                const int dy = std::abs((int)Y[r + x] - (int)pY_[r + x]);
                const int dc = (std::abs((int)Cb[r + x] - (int)pCb_[r + x]) + std::abs((int)Cr[r + x] - (int)pCr_[r + x])) / 2;
                motion[r + x] = (uint8_t)std::min(255, std::max(dy, dc));
            }
        }
        // a 3x3 mean of the differences: a moving edge reaches its neighbours (never half blended), while noise averages out
        std::vector<uint16_t> hs(n);
        for (int y = 0; y < h; y++) {
            const uint8_t* m = &motion[(size_t)y * w];
            uint16_t* d = &hs[(size_t)y * w];
            for (int x = 0; x < w; x++) d[x] = (uint16_t)((int)m[std::max(0, x - 1)] + (int)m[x] + (int)m[std::min(w - 1, x + 1)]);
        }
        for (int y = 0; y < h; y++) {
            const uint16_t* a = &hs[(size_t)std::max(0, y - 1) * w];
            const uint16_t* b = &hs[(size_t)y * w];
            const uint16_t* c = &hs[(size_t)std::min(h - 1, y + 1) * w];
            uint8_t* d = &motion[(size_t)y * w];
            for (int x = 0; x < w; x++) d[x] = (uint8_t)std::min(255, ((int)a[x] + (int)b[x] + (int)c[x] + 4) / 9);
        }
    }

    // ---- comb repair in moving areas (Full): a line that sticks out from both neighbours, where something moved
    if (full && have_) {
        const float thr = T * 1.2f + 6.f;
        std::vector<uint8_t> a(&Y[0], &Y[0] + w);   // the original line above
        for (int y = 1; y + 1 < h; y++) {
            uint8_t* b = &Y[(size_t)y * w];
            const uint8_t* c = &Y[(size_t)(y + 1) * w];
            const std::vector<uint8_t> orig(b, b + w);
            for (int x = 0; x < w; x++) {
                if (motion[(size_t)y * w + x] <= T) continue;
                const int up = (int)orig[x] - (int)a[x], dn = (int)orig[x] - (int)c[x];
                if (((up > 0) != (dn > 0)) || std::abs(up) < thr || std::abs(dn) < thr) continue;
                if (std::abs((int)a[x] - (int)c[x]) * 10 > std::min(std::abs(up), std::abs(dn)) * 7) continue;
                const size_t i = (size_t)y * w + x;
                Y[i] = (uint8_t)(((int)a[x] + (int)c[x] + 1) >> 1);
                Cb[i] = (uint8_t)(((int)Cb[i - w] + (int)Cb[i + w] + 1) >> 1);
                Cr[i] = (uint8_t)(((int)Cr[i - w] + (int)Cr[i + w] + 1) >> 1);
            }
            std::memcpy(a.data(), orig.data(), w);
        }
    }

    // ---- temporal noise reduction: the previous filtered picture is blended in where nothing moved
    if (have_ && p.temporal > 0.f) {
        const float amax = std::min(0.92f, (full ? 0.72f : 0.5f) * p.temporal);
        float aLut[256];
        for (int m = 0; m < 256; m++) aLut[m] = amax * std::min(1.f, std::max(0.f, 2.f - 2.f * (float)m / T));   // full weight below T/2, none at T
        for (size_t i = 0; i < n; i++) {
            const float a = aLut[motion[i]];
            if (a <= 0.f) continue;
            Y[i] = clamp8f((float)Y[i] + ((float)pY_[i] - (float)Y[i]) * a);
            Cb[i] = clamp8f((float)Cb[i] + ((float)pCb_[i] - (float)Cb[i]) * a);
            Cr[i] = clamp8f((float)Cr[i] + ((float)pCr_[i] - (float)Cr[i]) * a);
        }
    }

    // ---- edge-preserving spatial denoise (Full): 3x3 bilateral on luma; chroma follows the luma edges (bleed fix)
    if (full && p.spatial > 0.f) {
        float lut[256];
        rangeLut(lut, std::max(2.f, (1.6f * sigma_ + 2.f) * p.spatial));
        std::vector<uint8_t> Z(n);
        for (int y = 0; y < h; y++) {
            const uint8_t* r0 = &Y[(size_t)std::max(0, y - 1) * w];
            const uint8_t* r1 = &Y[(size_t)y * w];
            const uint8_t* r2 = &Y[(size_t)std::min(h - 1, y + 1) * w];
            uint8_t* d = &Z[(size_t)y * w];
            for (int x = 0; x < w; x++) {
                const int xl = std::max(0, x - 1), xr = std::min(w - 1, x + 1);
                const int c = r1[x];
                float sw = 1.f, sv = (float)c;
                const int px[8] = {r0[xl], r0[x], r0[xr], r1[xl], r1[xr], r2[xl], r2[x], r2[xr]};
                for (int k = 0; k < 8; k++) { const float wk = lut[std::abs(px[k] - c)] * ((k == 1 || k == 3 || k == 4 || k == 6) ? 1.f : 0.7f); sw += wk; sv += wk * (float)px[k]; }
                d[x] = clamp8f(sv / sw);
            }
        }
        Y.swap(Z);
    }
    if (full && p.chroma > 0.f) {
        float lut[256];
        rangeLut(lut, std::max(3.f, (2.f * sigma_ + 5.f) / p.chroma));   // a smaller step in luma ends the smoothing earlier
        std::vector<uint8_t> t1(n), t2(n);
        for (int pass = 0; pass < 2; pass++) {
            uint8_t* C = pass ? Cr.data() : Cb.data();
            for (int y = 0; y < h; y++) {   // horizontal, radius 4
                const uint8_t* yr = &Y[(size_t)y * w];
                const uint8_t* cr = C + (size_t)y * w;
                uint8_t* d = &t1[(size_t)y * w];
                for (int x = 0; x < w; x++) {
                    float sw = 0.f, sv = 0.f;
                    const int yc = yr[x];
                    for (int k = -4; k <= 4; k++) {
                        const int xx = std::min(w - 1, std::max(0, x + k));
                        const float wk = lut[std::abs((int)yr[xx] - yc)] * (1.f - 0.1f * (float)std::abs(k));
                        sw += wk; sv += wk * (float)cr[xx];
                    }
                    d[x] = clamp8f(sv / sw);
                }
            }
            for (int y = 0; y < h; y++) {   // vertical, radius 2
                uint8_t* d = &t2[(size_t)y * w];
                for (int x = 0; x < w; x++) {
                    float sw = 0.f, sv = 0.f;
                    const int yc = Y[(size_t)y * w + x];
                    for (int k = -2; k <= 2; k++) {
                        const int yy = std::min(h - 1, std::max(0, y + k));
                        const float wk = lut[std::abs((int)Y[(size_t)yy * w + x] - yc)] * (1.f - 0.25f * (float)std::abs(k));
                        sw += wk; sv += wk * (float)t1[(size_t)yy * w + x];
                    }
                    d[x] = clamp8f(sv / sw);
                }
            }
            std::memcpy(C, t2.data(), n);
        }
    }

    // keep the cleaned picture (before scaling and sharpening) as the reference for the next one
    pY_ = Y; pCb_ = Cb; pCr_ = Cr; have_ = true;

    // ---- scale to the output grid
    outputSize(w, h, p, ow, oh);
    const size_t on = (size_t)ow * oh;
    std::vector<float> tmp;
    std::vector<uint8_t> oY(on), oCb, oCr;
    scalePlane(Y.data(), w, h, oY.data(), ow, oh, full, tmp);
    oCb.resize(on); oCr.resize(on);
    scalePlane(Cb.data(), w, h, oCb.data(), ow, oh, false, tmp);
    scalePlane(Cr.data(), w, h, oCr.data(), ow, oh, false, tmp);

    // ---- sharpen luma (Full): unsharp with coring (noise is not boosted) and a clamp to the local range (no ringing)
    if (full && p.sharpen > 0.f) {
        const float amt = 0.75f * p.sharpen;
        const float core = 0.6f * sigma_ * ((float)ow / (float)w > 1.2f ? 1.3f : 1.f) + 0.5f;
        std::vector<uint8_t> S(on);
        for (int y = 0; y < oh; y++) {
            const uint8_t* r0 = &oY[(size_t)std::max(0, y - 1) * ow];
            const uint8_t* r1 = &oY[(size_t)y * ow];
            const uint8_t* r2 = &oY[(size_t)std::min(oh - 1, y + 1) * ow];
            uint8_t* d = &S[(size_t)y * ow];
            for (int x = 0; x < ow; x++) {
                const int xl = std::max(0, x - 1), xr = std::min(ow - 1, x + 1);
                const int c = r1[x];
                const int a0 = r0[xl], a1 = r0[x], a2 = r0[xr], b0 = r1[xl], b2 = r1[xr], c0 = r2[xl], c1 = r2[x], c2 = r2[xr];
                const float blur = (float)(a0 + 2 * a1 + a2 + 2 * b0 + 4 * c + 2 * b2 + c0 + 2 * c1 + c2) * (1.f / 16.f);
                float det = (float)c - blur;
                const float ad = std::fabs(det);
                det = ad <= core ? 0.f : (det > 0 ? ad - core : core - ad);
                const int lo = std::min(std::min(std::min(a0, a1), std::min(a2, b0)), std::min(std::min(b2, c0), std::min(c1, c2)));
                const int hi = std::max(std::max(std::max(a0, a1), std::max(a2, b0)), std::max(std::max(b2, c0), std::max(c1, c2)));
                const float v = (float)c + amt * 1.5f * det;
                d[x] = clamp8f(std::min((float)std::max(hi, c), std::max((float)std::min(lo, c), v)));
            }
        }
        oY.swap(S);
    }

    out.assign(on * 4, 255);
    toRgba(oY.data(), oCb.data(), oCr.data(), on, out.data());
    return true;
}

} // namespace dect2
