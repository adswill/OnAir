// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Image enhancement, geometry: slant (clock error) estimation and correction, phasing, line repair, line registration, resizing.
#include "enhance_util.h"
#include <numeric>

namespace dect2::image {
using namespace detail;

namespace {

// rows of one plane, each made zero mean and unit norm (flat rows stay zero); norm[y] = the rms before
std::vector<float> normRows(const ImageF& im, std::vector<float>* rms = nullptr) {
    const int w = im.w, h = im.h;
    std::vector<float> y = lumaPlane(im);
    if (rms) rms->assign((size_t)h, 0.f);
    for (int r = 0; r < h; r++) {
        float* p = y.data() + (size_t)r * w;
        double m = 0;
        for (int x = 0; x < w; x++) m += p[x];
        m /= w;
        double s = 0;
        for (int x = 0; x < w; x++) { p[x] -= (float)m; s += (double)p[x] * p[x]; }
        const double nrm = std::sqrt(s);
        if (rms) (*rms)[(size_t)r] = (float)std::sqrt(s / w);
        const float k = nrm > 1e-6 ? (float)(1.0 / nrm) : 0.f;
        for (int x = 0; x < w; x++) p[x] *= k;
    }
    return y;
}

// best d in [lo, hi] maximising sum a[x] * b[x + d] (circular); sub-pixel by a parabola through the peak; peak = the correlation
double bestShift(const float* a, const float* b, int w, int lo, int hi, double& peak) {
    int bd = 0;
    double bv = -1e30;
    std::vector<double> v((size_t)(hi - lo + 1));
    for (int d = lo; d <= hi; d++) {
        double s = 0;
        for (int x = 0; x < w; x++) s += (double)a[x] * b[wrapi(x + d, w)];
        v[(size_t)(d - lo)] = s;
        if (s > bv) { bv = s; bd = d; }
    }
    peak = bv;
    double frac = 0;
    if (bd > lo && bd < hi) {
        const double l = v[(size_t)(bd - 1 - lo)], c = v[(size_t)(bd - lo)], r = v[(size_t)(bd + 1 - lo)];
        const double den = l - 2 * c + r;
        if (den < -1e-12) frac = std::clamp(0.5 * (l - r) / den, -0.5, 0.5);
    }
    return bd + frac;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    const size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + (ptrdiff_t)m, v.end());
    double r = v[m];
    if (v.size() % 2 == 0) { r = (r + *std::max_element(v.begin(), v.begin() + (ptrdiff_t)m)) / 2; }
    return r;
}

// one plane shifted left by s px with linear interpolation: out[x] = in[x + s]
void shiftRow(const float* in, float* out, int w, double s, bool wrap) {
    const double fl = std::floor(s);
    const int i0 = (int)fl;
    const float f = (float)(s - fl);
    for (int x = 0; x < w; x++) {
        int a = x + i0, b = a + 1;
        if (wrap) { a = wrapi(a, w); b = wrapi(b, w); } else { a = clampi(a, 0, w - 1); b = clampi(b, 0, w - 1); }
        out[x] = in[a] * (1 - f) + in[b] * f;
    }
}

}  // namespace

SlantFit estimateSlant(const ImageF& im, int maxStep) {
    SlantFit f;
    const int w = im.w, h = im.h;
    if (w < 16 || h < 16) return f;
    const std::vector<float> y = normRows(im);
    auto row = [&](int r) { return y.data() + (size_t)r * w; };
    // the lag between the two lines compared grows in stages; each stage searches around the slope of the one before
    double s = 0;
    bool have = false;
    std::vector<double> lastSet;
    for (int L : {4, 32, 128, 512}) {
        if (L > h / 2 && L != 4) break;
        const int range = have ? std::max(3, (int)std::ceil(std::abs(s) * L * 0.25 + 2)) : maxStep * L;
        const int centre = have ? (int)std::lround(s * L) : 0;
        if (range > w / 3) continue;
        const int npairs = h - L;
        const int step = std::max(1, npairs / 300);
        std::vector<int> ys;
        for (int r = 0; r < npairs; r += step) ys.push_back(r);
        std::vector<double> sh(ys.size(), 0), pk(ys.size(), 0);
        ThreadPool::shared().parallelFor(ys.size(), [&](size_t i) {
            double p;
            sh[i] = bestShift(row(ys[i]), row(ys[i] + L), w, centre - range, centre + range, p);
            pk[i] = p;
        });
        std::vector<double> est;
        for (size_t i = 0; i < ys.size(); i++) if (pk[i] > 0.4) est.push_back(sh[i] / L);
        if (est.size() < 8 || est.size() < ys.size() / 5) { if (!have) return f; break; }
        s = median(est);
        have = true;
        lastSet = est;
        f.used = (int)est.size();
    }
    std::vector<double> dev;
    for (double e : lastSet) dev.push_back(std::abs(e - s));
    f.pxPerLine = s;
    f.rms = 1.4826 * median(dev);
    return f;
}

ImageF deslant(const ImageF& im, double pxPerLine, double offset, bool wrap) {
    ImageF o(im.w, im.h, im.ch);
    for (int c = 0; c < im.ch; c++) {
        const float* in = im.plane(c);
        float* out = o.plane(c);
        forRows(im.h, [&](int y0, int y1) {
            for (int y = y0; y < y1; y++) shiftRow(in + (size_t)y * im.w, out + (size_t)y * im.w, im.w, pxPerLine * y + offset, wrap);
        });
    }
    return o;
}

int findPhase(const ImageF& im, double bandPct) {
    const int w = im.w, h = im.h;
    if (w < 16 || h < 4) return 0;
    const std::vector<float> y = lumaPlane(im);
    std::vector<double> mean((size_t)w, 0), var((size_t)w, 0);
    for (int r = 0; r < h; r++) for (int x = 0; x < w; x++) mean[(size_t)x] += y[(size_t)r * w + x];
    double gm = 0;
    for (double& m : mean) { m /= h; gm += m; }
    gm /= w;
    for (int r = 0; r < h; r++) for (int x = 0; x < w; x++) { const double d = y[(size_t)r * w + x] - mean[(size_t)x]; var[(size_t)x] += d * d; }
    for (double& v : var) v /= h;
    // score of a column: its spread down the picture, less a bonus for standing out from the picture's mean level
    std::vector<double> sc((size_t)w);
    for (int x = 0; x < w; x++) sc[(size_t)x] = var[(size_t)x] - 0.25 * std::abs(mean[(size_t)x] - gm) * std::abs(mean[(size_t)x] - gm);
    const int bw = std::max(2, (int)std::lround(w * bandPct / 100.0));
    double run = 0;
    for (int x = 0; x < bw; x++) run += sc[(size_t)x];
    std::vector<double> band((size_t)w);
    for (int x = 0; x < w; x++) { band[(size_t)x] = run; run += sc[(size_t)wrapi(x + bw, w)] - sc[(size_t)x]; }
    int best = 0;
    for (int x = 1; x < w; x++) if (band[(size_t)x] < band[(size_t)best]) best = x;
    // a real sync bar or margin: clearly more constant than a typical band, and of a level unlike the picture
    std::vector<double> bv(band);
    std::nth_element(bv.begin(), bv.begin() + w / 2, bv.end());
    const double med = bv[(size_t)w / 2];
    double bm = 0, bvar = 0;
    for (int x = 0; x < bw; x++) { bm += mean[(size_t)wrapi(best + x, w)]; bvar += var[(size_t)wrapi(best + x, w)]; }
    bm /= bw; bvar /= bw;
    double vmed;
    { std::vector<double> vv(var); std::nth_element(vv.begin(), vv.begin() + w / 2, vv.end()); vmed = vv[(size_t)w / 2]; }
    if (!(band[(size_t)best] < 0.25 * med || bvar < 0.1 * vmed) || std::abs(bm - gm) < 0.15) return 0;
    // the band starts where its level begins: walk left over columns of the same level
    int start = best;
    for (int i = 0; i < w / 4; i++) {
        const int x = wrapi(start - 1, w);
        if (std::abs(mean[(size_t)x] - bm) < 0.5 * std::abs(gm - bm)) start = x; else break;
    }
    return start;
}

ImageF rotateColumns(const ImageF& im, int shift) {
    ImageF o(im.w, im.h, im.ch);
    const int s = wrapi(shift, std::max(1, im.w));
    for (int c = 0; c < im.ch; c++)
        for (int y = 0; y < im.h; y++) {
            const float* in = im.plane(c) + (size_t)y * im.w;
            float* out = o.plane(c) + (size_t)y * im.w;
            std::copy(in + s, in + im.w, out);
            std::copy(in, in + s, out + (im.w - s));
        }
    return o;
}

int repairLines(ImageF& im, double minCorr, std::vector<int>* fixed) {
    const int w = im.w, h = im.h;
    if (w < 8 || h < 5) return 0;
    std::vector<float> rms;
    const std::vector<float> y = normRows(im, &rms);
    auto dot = [&](int a, int b) {
        const float *p = y.data() + (size_t)a * w, *q = y.data() + (size_t)b * w;
        double s = 0;
        for (int x = 0; x < w; x++) s += (double)p[x] * q[x];
        return s;
    };
    // the best correlation of every line with a line up to 3 away
    std::vector<double> sc((size_t)h, 0);
    std::vector<char> flat((size_t)h, 0);
    for (int r = 0; r < h; r++) flat[(size_t)r] = rms[(size_t)r] < 0.004f;
    ThreadPool::shared().parallelFor((size_t)h, [&](size_t ri) {
        const int r = (int)ri;
        double b = -1;
        for (int k = -3; k <= 3; k++) if (k && r + k >= 0 && r + k < h && !flat[(size_t)(r + k)]) b = std::max(b, dot(r, r + k));
        sc[ri] = b;
    });
    std::vector<double> good;
    for (int r = 0; r < h; r++) if (!flat[(size_t)r]) good.push_back(sc[(size_t)r]);
    if (good.size() < 4) return 0;
    std::nth_element(good.begin(), good.begin() + (ptrdiff_t)good.size() / 2, good.end());
    const double med = good[good.size() / 2];
    if (med < 0.2) return 0;   // a picture of noise: nothing to compare against
    const double thr = minCorr * med;
    // mean level of each line against the neighbourhood (a flat line in a busy region, a saturated burst)
    std::vector<float> mean((size_t)h, 0);
    {
        const std::vector<float> l = lumaPlane(im);
        for (int r = 0; r < h; r++) { double m = 0; for (int x = 0; x < w; x++) m += l[(size_t)r * w + x]; mean[(size_t)r] = (float)(m / w); }
    }
    std::vector<char> bad((size_t)h, 0);
    for (int r = 0; r < h; r++) {
        int busy = 0, n = 0;
        for (int k = -3; k <= 3; k++) if (k && r + k >= 0 && r + k < h) { n++; if (!flat[(size_t)(r + k)]) busy++; }
        if (flat[(size_t)r]) bad[(size_t)r] = busy * 2 > n;
        else bad[(size_t)r] = sc[(size_t)r] < thr;
    }
    int nfix = 0;
    for (int r = 0; r < h;) {
        if (!bad[(size_t)r]) { r++; continue; }
        int e = r;
        while (e + 1 < h && bad[(size_t)(e + 1)]) e++;
        const int a = r - 1, b = e + 1;
        if (e - r + 1 > h / 4) { r = e + 1; continue; }   // most of the picture: leave it
        for (int q = r; q <= e; q++) {
            const float t = (a >= 0 && b < h) ? float(q - a) / float(b - a) : 0.f;
            for (int c = 0; c < im.ch; c++) {
                float* p = im.plane(c);
                for (int x = 0; x < w; x++) {
                    const float va = a >= 0 ? p[(size_t)a * w + x] : p[(size_t)b * w + x];
                    const float vb = b < h ? p[(size_t)b * w + x] : va;
                    p[(size_t)q * w + x] = va * (1 - t) + vb * t;
                }
            }
            if (fixed) fixed->push_back(q);
            nfix++;
        }
        r = e + 1;
    }
    return nfix;
}

int registerLines(ImageF& im, int maxShift) {
    const int w = im.w, h = im.h;
    if (w < 16 || h < 5) return 0;
    // the shift of every line against the line before (independent jitter makes these differences of neighbours), chained into the
    // position of every line; the slow part of that chain (slant, curved content) is the running median, the jitter is the rest
    const std::vector<float> y = normRows(im);
    std::vector<double> d((size_t)h, 0);
    ThreadPool::shared().parallelFor((size_t)h, [&](size_t ri) {
        const int r = (int)ri;
        if (r == 0) return;
        double pk;
        const double s = bestShift(y.data() + (size_t)(r - 1) * w, y.data() + (size_t)r * w, w, -2 * maxShift, 2 * maxShift, pk);
        if (pk > 0.5) d[ri] = s;
    });
    std::vector<double> pos((size_t)h, 0);
    for (int r = 1; r < h; r++) pos[(size_t)r] = pos[(size_t)(r - 1)] + d[(size_t)r];
    const int win = 15;
    std::vector<double> trend((size_t)h), b;
    for (int r = 0; r < h; r++) {
        b.clear();
        for (int k = -win; k <= win; k++) b.push_back(pos[(size_t)clampi(r + k, 0, h - 1)]);
        std::nth_element(b.begin(), b.begin() + win, b.end());
        trend[(size_t)r] = b[(size_t)win];
    }
    int moved = 0;
    std::vector<float> tmp((size_t)w);
    for (int r = 0; r < h; r++) {
        const double s = pos[(size_t)r] - trend[(size_t)r];
        if (std::abs(s) < 0.25 || std::abs(s) > maxShift + 0.5) continue;
        for (int c = 0; c < im.ch; c++) {
            float* p = im.plane(c) + (size_t)r * w;
            shiftRow(p, tmp.data(), w, s, true);
            std::copy(tmp.begin(), tmp.end(), p);
        }
        moved++;
    }
    return moved;
}

ImageF resize(const ImageF& im0, int nw, int nh) {
    if (nw <= 0 || nh <= 0 || im0.empty()) return ImageF();
    const ImageF* src = &im0;
    ImageF pre;
    const float fx = (float)im0.w / nw, fy = (float)im0.h / nh;
    if (fx > 2.f || fy > 2.f) { pre = im0; gaussian(pre, 0.5f * std::max(fx, fy)); src = &pre; }
    const ImageF& im = *src;
    ImageF o(nw, nh, im.ch);
    for (int c = 0; c < im.ch; c++) {
        const float* in = im.plane(c);
        float* out = o.plane(c);
        forRows(nh, [&](int y0, int y1) {
            for (int y = y0; y < y1; y++) {
                const float sy = std::clamp((y + 0.5f) * fy - 0.5f, 0.f, (float)(im.h - 1));
                const int ya = (int)sy, yb = std::min(ya + 1, im.h - 1);
                const float ty = sy - ya;
                for (int x = 0; x < nw; x++) {
                    const float sx = std::clamp((x + 0.5f) * fx - 0.5f, 0.f, (float)(im.w - 1));
                    const int xa = (int)sx, xb = std::min(xa + 1, im.w - 1);
                    const float tx = sx - xa;
                    const float top = in[(size_t)ya * im.w + xa] * (1 - tx) + in[(size_t)ya * im.w + xb] * tx;
                    const float bot = in[(size_t)yb * im.w + xa] * (1 - tx) + in[(size_t)yb * im.w + xb] * tx;
                    out[(size_t)y * nw + x] = top * (1 - ty) + bot * ty;
                }
            }
        });
    }
    return o;
}

ImageF iocWidth(const ImageF& im, int ioc) {
    if (ioc <= 0) return im;
    const int nw = (int)std::lround(ioc * 3.14159265358979);
    return nw == im.w ? im : resize(im, nw, im.h);
}

}  // namespace dect2::image
