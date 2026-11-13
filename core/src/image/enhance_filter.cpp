// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Image enhancement, noise and detail: switching median, bilateral, non-local means, destripe and deband, temporal averaging,
// unsharp mask, Lanczos x2 without ringing, chroma fix, grey world, deinterlace.
#include "enhance_util.h"

namespace dect2::image {
using namespace detail;

// ---------------------------------------------------------------- impulses

int switchingMedian(ImageF& im, float thr) {
    const int w = im.w, h = im.h;
    if (w < 3 || h < 3) return 0;
    int total = 0;
    for (int c = 0; c < im.ch; c++) {
        const std::vector<float> in(im.plane(c), im.plane(c) + (size_t)w * h);
        float* out = im.plane(c);
        const int bands = (h + 31) / 32;
        std::vector<int> cnt((size_t)bands, 0);
        ThreadPool::shared().parallelFor((size_t)bands, [&](size_t bi) {
            const int y0 = (int)bi * 32, y1 = std::min(h, y0 + 32);
            float v[9];
            for (int y = y0; y < y1; y++)
                for (int x = 0; x < w; x++) {
                    int k = 0, alike = 0;
                    const float p = in[(size_t)y * w + x];
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            const float q = in[(size_t)clampi(y + dy, 0, h - 1) * w + clampi(x + dx, 0, w - 1)];
                            v[k++] = q;
                            if ((dx || dy) && std::abs(q - p) < 0.5f * thr) alike++;
                        }
                    if (alike >= 2) continue;   // part of a line or a dot of 3 or more pixels: kept (thin chart lines, text)
                    std::nth_element(v, v + 4, v + 9);
                    const float m = v[4];
                    if (std::abs(p - m) > thr) { out[(size_t)y * w + x] = m; cnt[bi]++; }
                }
        });
        for (int n : cnt) total += n;
    }
    return total;
}

// ---------------------------------------------------------------- edge-preserving denoise

ImageF bilateral(const ImageF& im, float sigmaS, float sigmaR) {
    const int w = im.w, h = im.h, r = std::max(1, (int)std::ceil(2.f * sigmaS));
    ImageF o(w, h, im.ch);
    std::vector<float> ws((size_t)((2 * r + 1) * (2 * r + 1)));
    for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) ws[(size_t)((dy + r) * (2 * r + 1) + dx + r)] = std::exp(-0.5f * (dx * dx + dy * dy) / (sigmaS * sigmaS));
    float rl[1025];   // range weight by |difference| in 1/1024 steps
    for (int i = 0; i <= 1024; i++) { const float d = i / 1024.f; rl[i] = std::exp(-0.5f * d * d / (sigmaR * sigmaR)); }
    for (int c = 0; c < im.ch; c++) {
        const float* in = im.plane(c);
        float* out = o.plane(c);
        forRows(h, [&](int y0, int y1) {
            for (int y = y0; y < y1; y++)
                for (int x = 0; x < w; x++) {
                    const float p = in[(size_t)y * w + x];
                    float s = 0, sw = 0;
                    const bool inner = x >= r && x + r < w && y >= r && y + r < h;
                    for (int dy = -r; dy <= r; dy++) {
                        const int yy = inner ? y + dy : clampi(y + dy, 0, h - 1);
                        const float* row = in + (size_t)yy * w;
                        const float* wr = ws.data() + (size_t)((dy + r) * (2 * r + 1) + r);
                        for (int dx = -r; dx <= r; dx++) {
                            const float q = row[inner ? x + dx : clampi(x + dx, 0, w - 1)];
                            const float wt = wr[dx] * rl[std::min(1024, (int)(std::abs(q - p) * 1024.f))];
                            s += wt * q; sw += wt;
                        }
                    }
                    out[(size_t)y * w + x] = s / sw;
                }
        });
    }
    return o;
}

ImageF nlMeans(const ImageF& im, float hp, int sr, int pr) {
    const int w = im.w, h = im.h;
    ImageF o(w, h, im.ch);
    const float inv = 1.f / (hp * hp);
    const int halo = sr + pr;
    const int pn = (2 * pr + 1) * (2 * pr + 1);
    for (int c = 0; c < im.ch; c++) {
        const float* in = im.plane(c);
        float* out = o.plane(c);
        auto at = [&](int x, int y) { return in[(size_t)clampi(y, 0, h - 1) * w + clampi(x, 0, w - 1)]; };
        forRows(h, [&](int y0, int y1) {
            const int bh = y1 - y0, eh = bh + 2 * pr;   // rows of the band plus the patch margin
            std::vector<float> d((size_t)eh * w), dh((size_t)eh * w), acc((size_t)bh * w, 0.f), wsum((size_t)bh * w, 0.f), wmax((size_t)bh * w, 0.f);
            (void)halo;
            for (int oy = -sr; oy <= sr; oy++)
                for (int ox = -sr; ox <= sr; ox++) {
                    if (!ox && !oy) continue;
                    for (int r = 0; r < eh; r++) {
                        const int y = y0 - pr + r;
                        for (int x = 0; x < w; x++) { const float e = at(x, y) - at(x + ox, y + oy); d[(size_t)r * w + x] = e * e; }
                    }
                    // patch sum: horizontal then vertical box
                    for (int r = 0; r < eh; r++) {
                        const float* dr = d.data() + (size_t)r * w;
                        float* hr = dh.data() + (size_t)r * w;
                        for (int x = 0; x < w; x++) { float s = 0; for (int k = -pr; k <= pr; k++) s += dr[clampi(x + k, 0, w - 1)]; hr[x] = s; }
                    }
                    for (int r = 0; r < bh; r++) {
                        const int y = y0 + r;
                        for (int x = 0; x < w; x++) {
                            float s = 0;
                            for (int k = 0; k <= 2 * pr; k++) s += dh[(size_t)(r + k) * w + x];
                            const float dist = s / pn;
                            const float wt = std::exp(-std::max(0.f, dist) * inv);
                            const size_t i = (size_t)r * w + x;
                            acc[i] += wt * at(x + ox, y + oy); wsum[i] += wt; wmax[i] = std::max(wmax[i], wt);
                        }
                    }
                }
            for (int r = 0; r < bh; r++)
                for (int x = 0; x < w; x++) {
                    const size_t i = (size_t)r * w + x;
                    const float wc = wmax[i] > 0 ? wmax[i] : 1.f;
                    out[(size_t)(y0 + r) * w + x] = (acc[i] + wc * in[(size_t)(y0 + r) * w + x]) / (wsum[i] + wc);
                }
        }, 48);
    }
    return o;
}

ImageF denoise(const ImageF& im, int strength, bool nlm) {
    const float s = std::clamp(strength, 0, 100) / 100.f;
    if (s <= 0) return im;
    if (nlm) return nlMeans(im, 0.03f + 0.17f * s, 4, 1);
    return bilateral(im, 0.8f + 1.2f * s, 0.03f + 0.17f * s);
}

// ---------------------------------------------------------------- stripes and bands

namespace {
// running median of v over window (odd), edge clamped
std::vector<double> runMedian(const std::vector<double>& v, int win) {
    const int n = (int)v.size(), r = win / 2;
    std::vector<double> o((size_t)n), b;
    for (int i = 0; i < n; i++) {
        b.clear();
        for (int k = -r; k <= r; k++) b.push_back(v[(size_t)clampi(i + k, 0, n - 1)]);
        std::nth_element(b.begin(), b.begin() + r, b.end());
        o[(size_t)i] = b[(size_t)r];
    }
    return o;
}

// per line (rows = true) or per column gain and offset pulled to the running median of the neighbours
void equalise(ImageF& im, int win, bool rows) {
    const int w = im.w, h = im.h, n = rows ? h : w, len = rows ? w : h;
    if (n < 5 || len < 2) return;
    for (int c = 0; c < im.ch; c++) {
        float* p = im.plane(c);
        auto px = [&](int i, int j) -> float& { return rows ? p[(size_t)i * w + j] : p[(size_t)j * w + i]; };
        std::vector<double> m((size_t)n), s((size_t)n);
        for (int i = 0; i < n; i++) {
            double a = 0, q = 0;
            for (int j = 0; j < len; j++) { const double v = px(i, j); a += v; q += v * v; }
            a /= len;
            m[(size_t)i] = a; s[(size_t)i] = std::sqrt(std::max(0.0, q / len - a * a));
        }
        const auto mt = runMedian(m, win | 1), st = runMedian(s, win | 1);
        for (int i = 0; i < n; i++) {
            const double g = s[(size_t)i] > 1e-4 ? std::clamp(st[(size_t)i] / s[(size_t)i], 0.5, 2.0) : 1.0;
            for (int j = 0; j < len; j++) { float& v = px(i, j); v = clamp01((float)((v - m[(size_t)i]) * g + mt[(size_t)i])); }
        }
    }
}
}  // namespace

void destripe(ImageF& im, int window) { equalise(im, window, true); }
void deband(ImageF& im, int window) { equalise(im, window, false); }

// ---------------------------------------------------------------- repeated transmissions

ImageF temporalAverage(const std::vector<ImageF>& copies, int maxShift, std::vector<int>* dxo, std::vector<int>* dyo) {
    if (copies.empty()) return ImageF();
    const ImageF& ref = copies[0];
    const int w = ref.w, h = ref.h;
    std::vector<const ImageF*> use{&ref};
    std::vector<int> dxs{0}, dys{0};
    const std::vector<float> ry = lumaPlane(ref);
    for (size_t k = 1; k < copies.size(); k++) {
        const ImageF& c = copies[k];
        if (c.w != w || c.h != h || c.ch != ref.ch) continue;
        const std::vector<float> cy = lumaPlane(c);
        // SAD over a sparse grid of the reference; coarse step 2 search, then +-1 refine
        auto sad = [&](int dx, int dy) {
            double s = 0; int n = 0;
            for (int y = std::max(0, -dy) + 2; y < std::min(h, h - dy) - 2; y += 3)
                for (int x = 1; x < w; x += 3) { s += std::abs(ry[(size_t)y * w + x] - cy[(size_t)(y + dy) * w + wrapi(x + dx, w)]); n++; }
            return n ? s / n : 1e9;
        };
        const int ms = std::min(maxShift, std::min(w, h) / 4);
        std::vector<std::pair<int, int>> cand;
        for (int dy = -ms; dy <= ms; dy += 2) for (int dx = -ms; dx <= ms; dx += 2) cand.push_back({dx, dy});
        std::vector<double> v(cand.size());
        ThreadPool::shared().parallelFor(cand.size(), [&](size_t i) { v[i] = sad(cand[i].first, cand[i].second); });
        size_t bi = 0;
        for (size_t i = 1; i < v.size(); i++) if (v[i] < v[bi]) bi = i;
        int bx = cand[bi].first, by = cand[bi].second;
        double bv = v[bi];
        for (int it = 0; it < 4; it++) {   // +-1 refinement around the best, until it stays
            int nx = bx, ny = by;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                const double s = sad(bx + dx, by + dy);
                if (s < bv) { bv = s; nx = bx + dx; ny = by + dy; }
            }
            if (nx == bx && ny == by) break;
            bx = nx; by = ny;
        }
        use.push_back(&c); dxs.push_back(bx); dys.push_back(by);
    }
    if (dxo) *dxo = dxs;
    if (dyo) *dyo = dys;
    ImageF o(w, h, ref.ch);
    const size_t m = use.size();
    for (int c = 0; c < ref.ch; c++) {
        float* out = o.plane(c);
        forRows(h, [&](int y0, int y1) {
            std::vector<float> s(m);
            for (int y = y0; y < y1; y++)
                for (int x = 0; x < w; x++) {
                    size_t k = 0;
                    for (size_t i = 0; i < m; i++) {
                        const int yy = y + dys[i];
                        if (yy < 0 || yy >= h) continue;
                        s[k++] = use[i]->plane(c)[(size_t)yy * w + wrapi(x + dxs[i], w)];
                    }
                    float r;
                    if (k >= 3) { std::nth_element(s.begin(), s.begin() + (ptrdiff_t)k / 2, s.begin() + (ptrdiff_t)k); r = s[k / 2]; if (k % 2 == 0) r = (r + *std::max_element(s.begin(), s.begin() + (ptrdiff_t)k / 2)) / 2; }
                    else { r = 0; for (size_t i = 0; i < k; i++) r += s[i]; r = k ? r / k : 0; }
                    out[(size_t)y * w + x] = r;
                }
        });
    }
    return o;
}

// ---------------------------------------------------------------- detail

void unsharp(ImageF& im, float radius, float amount, float threshold) {
    if (amount <= 0) return;
    const auto k = gaussKernel(std::max(0.3f, radius));
    std::vector<float> b((size_t)im.w * im.h);
    for (int c = 0; c < im.ch; c++) {
        float* p = im.plane(c);
        convolvePlane(p, b.data(), im.w, im.h, k);
        for (size_t i = 0; i < b.size(); i++) {
            const float d = p[i] - b[i];
            if (std::abs(d) > threshold) p[i] = clamp01(p[i] + amount * d);
        }
    }
}

namespace {
float lanczos(float x) {
    const float a = 3.f;
    if (x == 0) return 1.f;
    if (std::abs(x) >= a) return 0.f;
    const float px = 3.14159265f * x;
    return a * std::sin(px) * std::sin(px / a) / (px * px);
}
}  // namespace

ImageF lanczos2x(const ImageF& im, float sharpen) {
    const int w = im.w, h = im.h, W = 2 * w, H = 2 * h;
    ImageF o(W, H, im.ch);
    // the output pixel x maps to source (x + 0.5) / 2 - 0.5: phase 0.25 or 0.75, so two fixed 6-tap kernels
    float k[2][6];
    int base[2];
    for (int ph = 0; ph < 2; ph++) {
        const float s = ph ? 0.25f : -0.25f;   // source offset of the even (-0.25) and odd (+0.25) outputs
        const int b0 = (int)std::floor(s) - 2;
        base[ph] = b0;
        float sum = 0;
        for (int t = 0; t < 6; t++) { k[ph][t] = lanczos(s - (b0 + t)); sum += k[ph][t]; }
        for (int t = 0; t < 6; t++) k[ph][t] /= sum;
    }
    for (int c = 0; c < im.ch; c++) {
        const float* in = im.plane(c);
        std::vector<float> hz((size_t)W * h);
        forRows(h, [&](int y0, int y1) {
            for (int y = y0; y < y1; y++)
                for (int X = 0; X < W; X++) {
                    const int ph = X & 1, sx = X >> 1;
                    float s = 0;
                    for (int t = 0; t < 6; t++) s += k[ph][t] * in[(size_t)y * w + clampi(sx + base[ph] + t, 0, w - 1)];
                    hz[(size_t)y * W + X] = s;
                }
        });
        float* out = o.plane(c);
        forRows(H, [&](int y0, int y1) {
            for (int Y = y0; Y < y1; Y++) {
                const int ph = Y & 1, sy = Y >> 1;
                for (int X = 0; X < W; X++) {
                    float s = 0;
                    for (int t = 0; t < 6; t++) s += k[ph][t] * hz[(size_t)clampi(sy + base[ph] + t, 0, h - 1) * W + X];
                    // no ringing: clamp to the range of the 2 x 2 source pixels around
                    const int xa = clampi((X >> 1) + ((X & 1) ? 0 : -1), 0, w - 1), xb = clampi(xa + 1, 0, w - 1);
                    const int ya = clampi(sy + (ph ? 0 : -1), 0, h - 1), yb = clampi(ya + 1, 0, h - 1);
                    const float a = in[(size_t)ya * w + xa], b = in[(size_t)ya * w + xb], cc = in[(size_t)yb * w + xa], d = in[(size_t)yb * w + xb];
                    s = std::clamp(s, std::min(std::min(a, b), std::min(cc, d)), std::max(std::max(a, b), std::max(cc, d)));
                    out[(size_t)Y * W + X] = s;
                }
            }
        });
    }
    if (sharpen > 0) unsharp(o, 1.2f, sharpen, 0.01f);
    return o;
}

double chromaFix(ImageF& im, float sigmaC, int maxShift) {
    if (im.ch != 3 || im.w < 16) return 0;
    const int w = im.w, h = im.h;
    toYCbCr(im);
    // the chroma lag: the shift that best lines up the horizontal gradient magnitude of the chroma with that of the luma
    std::vector<double> sc((size_t)(2 * maxShift + 1), 0);
    for (int d = -maxShift; d <= maxShift; d++) {
        double s = 0;
        for (int y = 0; y < h; y += 2)
            for (int x = maxShift + 1; x < w - maxShift - 1; x++) {
                const size_t i = (size_t)y * w + x;
                const float ly = std::abs(im.plane(0)[i + 1] - im.plane(0)[i - 1]);
                const size_t j = i + (size_t)(ptrdiff_t)d;
                const float lc = std::abs(im.plane(1)[j + 1] - im.plane(1)[j - 1]) + std::abs(im.plane(2)[j + 1] - im.plane(2)[j - 1]);
                s += (double)ly * lc;
            }
        sc[(size_t)(d + maxShift)] = s;
    }
    int bd = 0;
    for (int d = -maxShift; d <= maxShift; d++) if (sc[(size_t)(d + maxShift)] > sc[(size_t)(bd + maxShift)]) bd = d;
    double shift = bd;
    if (bd > -maxShift && bd < maxShift) {
        const double l = sc[(size_t)(bd - 1 + maxShift)], c = sc[(size_t)(bd + maxShift)], r = sc[(size_t)(bd + 1 + maxShift)];
        const double den = l - 2 * c + r;
        if (den < 0) shift += std::clamp(0.5 * (l - r) / den, -0.5, 0.5);
    }
    if (std::abs(shift) >= 0.5) {
        // chroma at x + shift belongs to luma at x
        std::vector<float> tmp((size_t)w);
        for (int c = 1; c < 3; c++)
            for (int y = 0; y < h; y++) {
                float* row = im.plane(c) + (size_t)y * w;
                const double fl = std::floor(shift);
                const float f = (float)(shift - fl);
                for (int x = 0; x < w; x++) {
                    const int a = clampi(x + (int)fl, 0, w - 1), b = clampi(x + (int)fl + 1, 0, w - 1);
                    tmp[(size_t)x] = row[a] * (1 - f) + row[b] * f;
                }
                std::copy(tmp.begin(), tmp.end(), row);
            }
    }
    // chroma smoothed far more than luma
    const auto k = gaussKernel(sigmaC);
    std::vector<float> o((size_t)w * h);
    for (int c = 1; c < 3; c++) { convolvePlane(im.plane(c), o.data(), w, h, k); std::copy(o.begin(), o.end(), im.plane(c)); }
    fromYCbCr(im);
    return shift;
}

void greyWorld(ImageF& im, float maxGain) {
    if (im.ch != 3) return;
    const size_t n = (size_t)im.w * im.h;
    double m[3] = {0, 0, 0};
    for (int c = 0; c < 3; c++) { const float* p = im.plane(c); for (size_t i = 0; i < n; i++) m[c] += p[i]; m[c] /= (double)n; }
    const double all = (m[0] + m[1] + m[2]) / 3;
    if (all < 1e-3) return;
    for (int c = 0; c < 3; c++) {
        const float g = (float)std::clamp(m[c] > 1e-4 ? all / m[c] : 1.0, 1.0 / maxGain, (double)maxGain);
        float* p = im.plane(c);
        for (size_t i = 0; i < n; i++) p[i] = clamp01(p[i] * g);
    }
}

int deinterlace(ImageF& im, float thr) {
    const int w = im.w, h = im.h;
    int n = 0;
    for (int c = 0; c < im.ch; c++) {
        float* p = im.plane(c);
        for (int y = 1; y + 1 < h; y += 2)
            for (int x = 0; x < w; x++) {
                const float a = p[(size_t)(y - 1) * w + x], b = p[(size_t)(y + 1) * w + x], v = p[(size_t)y * w + x];
                const float avg = 0.5f * (a + b);
                if (std::abs(v - avg) > thr && std::abs(a - b) < thr) { p[(size_t)y * w + x] = avg; if (c == 0) n++; }
            }
    }
    return n;
}

}  // namespace dect2::image
