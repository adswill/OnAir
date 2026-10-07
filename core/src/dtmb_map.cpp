// DTMB constellation mapping and soft demapping (see dtmb_map.h).
#include "dect2/dtmb_map.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace dect2::dtmb {

void mapSymbols(Mapping m, const uint8_t* bits, size_t nSym, cf32* out) {
    const QamPoint* pts = qamPoints(m);
    const int bps = bitsPerSymbol(m);
    for (size_t i = 0; i < nSym; i++) {
        int label = 0;
        for (int b = 0; b < bps; b++) label |= (bits[i * (size_t)bps + (size_t)b] & 1) << b;
        out[i] = cf32(pts[label].re, pts[label].im);
    }
}

namespace {

// levels of one axis (unit power constellation) and the label bits of each level
struct Axis {
    int bits = 0, n = 0;
    float level[8];
    uint8_t lab[8];
};

const Axis& axisOf(Mapping m) {
    static const std::array<Axis, 3> axes = [] {
        std::array<Axis, 3> a;
        const double l4[2] = {-4.5, 4.5}, l16[4] = {-6, -2, 2, 6}, l64[8] = {-7, -5, -3, -1, 1, 3, 5, 7};
        const double norm[3] = {std::sqrt(40.5), std::sqrt(40.0), std::sqrt(42.0)};
        const double* lv[3] = {l4, l16, l64};
        for (int k = 0; k < 3; k++) {
            a[(size_t)k].bits = k + 1; a[(size_t)k].n = 1 << (k + 1);
            for (int i = 0; i < a[(size_t)k].n; i++) { a[(size_t)k].level[i] = (float)(lv[k][i] / norm[k]); a[(size_t)k].lab[i] = (uint8_t)(i ^ (i >> 1)); }
        }
        return a;
    }();
    return axes[(size_t)(m == Mapping::Qam16 ? 1 : m == Mapping::Qam64 ? 2 : 0)];
}

void axisLlr(const Axis& ax, float y, float inv, float* llr) {
    float d[8];
    for (int i = 0; i < ax.n; i++) { const float e = y - ax.level[i]; d[i] = e * e; }
    for (int b = 0; b < ax.bits; b++) {
        float d0 = std::numeric_limits<float>::max(), d1 = d0;
        for (int i = 0; i < ax.n; i++) { if ((ax.lab[i] >> b) & 1) d1 = std::min(d1, d[i]); else d0 = std::min(d0, d[i]); }
        llr[b] = (d1 - d0) * inv;
    }
}

} // namespace

void demapSymbol(Mapping m, cf32 x, float var, float* llr) {
    const float inv = 1.f / std::max(var, 1e-12f);
    if (m == Mapping::Qam32) {
        const QamPoint* pts = qamPoints(Mapping::Qam32);
        float d0[5], d1[5];
        for (int b = 0; b < 5; b++) d0[b] = d1[b] = std::numeric_limits<float>::max();
        for (int l = 0; l < 32; l++) {
            const float er = x.real() - pts[l].re, ei = x.imag() - pts[l].im;
            const float d = er * er + ei * ei;
            for (int b = 0; b < 5; b++) { float& r = ((l >> b) & 1) ? d1[b] : d0[b]; r = std::min(r, d); }
        }
        for (int b = 0; b < 5; b++) llr[b] = (d1[b] - d0[b]) * inv;
        return;
    }
    const Axis& ax = axisOf(m);
    axisLlr(ax, x.real(), inv, llr);
    axisLlr(ax, x.imag(), inv, llr + ax.bits);
}

namespace {

// Max-log LLR of every axis bit at unit noise variance, tabulated over the received level: tab[bit][i] for y = (i - kHalf + 0.5) * kStep
constexpr int kAxisBins = 1024;
constexpr float kAxisRange = 1.6f;                 // levels beyond +-range clamp (unit power constellations reach 1.2)
struct AxisTables {
    float t[3][kAxisBins];
    float inv;                                     // bins per unit
    AxisTables(const Axis& ax) {
        inv = (float)kAxisBins / (2 * kAxisRange);
        for (int i = 0; i < kAxisBins; i++) {
            const float y = ((float)i + 0.5f) / inv - kAxisRange;
            float l[3] = {0, 0, 0};
            axisLlr(ax, y, 1.f, l);
            for (int b = 0; b < ax.bits; b++) t[b][i] = l[b];
        }
    }
};
const AxisTables& tablesOf(Mapping m) {
    static const AxisTables t4(axisOf(Mapping::Qam4)), t16(axisOf(Mapping::Qam16)), t64(axisOf(Mapping::Qam64));
    return m == Mapping::Qam16 ? t16 : m == Mapping::Qam64 ? t64 : t4;
}

// 32QAM: the LLRs of the five bits on a grid over the plane
constexpr int kGrid = 80;
constexpr float kGridRange = 1.5f;
struct Grid32 {
    std::vector<float> t;   // [bit][iy][ix]
    float inv;
    Grid32() : t((size_t)5 * kGrid * kGrid) {
        inv = (float)kGrid / (2 * kGridRange);
        const QamPoint* pts = qamPoints(Mapping::Qam32);
        for (int iy = 0; iy < kGrid; iy++) for (int ix = 0; ix < kGrid; ix++) {
            const float x = ((float)ix + 0.5f) / inv - kGridRange, y = ((float)iy + 0.5f) / inv - kGridRange;
            float d0[5], d1[5];
            for (int b = 0; b < 5; b++) d0[b] = d1[b] = std::numeric_limits<float>::max();
            for (int l = 0; l < 32; l++) {
                const float er = x - pts[l].re, ei = y - pts[l].im, d = er * er + ei * ei;
                for (int b = 0; b < 5; b++) { float& r = ((l >> b) & 1) ? d1[b] : d0[b]; r = std::min(r, d); }
            }
            for (int b = 0; b < 5; b++) t[((size_t)b * kGrid + (size_t)iy) * kGrid + (size_t)ix] = d1[b] - d0[b];
        }
    }
};
} // namespace

void demapBlock(Mapping m, const cf32* x, const float* var, size_t n, float* llr) {
    if (m == Mapping::Qam32) {
        static const Grid32 g;
        for (size_t i = 0; i < n; i++) {
            const float inv = 1.f / std::max(var[i], 1e-12f);
            // bilinear interpolation between the cell centres
            float gx = (x[i].real() + kGridRange) * g.inv - 0.5f, gy = (x[i].imag() + kGridRange) * g.inv - 0.5f;
            gx = std::max(0.f, std::min((float)(kGrid - 1) - 1e-3f, gx)); gy = std::max(0.f, std::min((float)(kGrid - 1) - 1e-3f, gy));
            const int ix = (int)gx, iy = (int)gy;
            const float fx = gx - (float)ix, fy = gy - (float)iy;
            float* o = llr + i * 5;
            for (int b = 0; b < 5; b++) {
                const float* tb = &g.t[(size_t)b * kGrid * kGrid];
                const float a = tb[iy * kGrid + ix], bb = tb[iy * kGrid + ix + 1], c = tb[(iy + 1) * kGrid + ix], d = tb[(iy + 1) * kGrid + ix + 1];
                o[b] = ((a + (bb - a) * fx) * (1.f - fy) + (c + (d - c) * fx) * fy) * inv;
            }
        }
        return;
    }
    const AxisTables& t = tablesOf(m);
    const int bits = m == Mapping::Qam64 ? 3 : m == Mapping::Qam16 ? 2 : 1;
    for (size_t i = 0; i < n; i++) {
        const float inv = 1.f / std::max(var[i], 1e-12f);
        int ix = (int)((x[i].real() + kAxisRange) * t.inv), iy = (int)((x[i].imag() + kAxisRange) * t.inv);
        ix = std::max(0, std::min(kAxisBins - 1, ix)); iy = std::max(0, std::min(kAxisBins - 1, iy));
        float* o = llr + i * (size_t)(2 * bits);
        for (int b = 0; b < bits; b++) { o[b] = t.t[b][ix] * inv; o[bits + b] = t.t[b][iy] * inv; }
    }
}

double decisionError(Mapping m, const cf32* x, size_t n, size_t stride) {
    if (n == 0 || stride == 0) return 0;
    double e = 0;
    size_t cnt = 0;
    if (m == Mapping::Qam32) {
        const QamPoint* pts = qamPoints(m);
        for (size_t i = 0; i < n; i += stride * 4) {
            float best = 1e30f;
            for (int k = 0; k < 32; k++) { const float dr = x[i].real() - pts[k].re, di = x[i].imag() - pts[k].im; best = std::min(best, dr * dr + di * di); }
            e += best; cnt++;
        }
        return cnt ? e / (double)cnt : 0.0;
    }
    const Axis& ax = axisOf(m);
    // levels are equally spaced: the nearest one is found by rounding
    const float step = ax.level[1] - ax.level[0], first = ax.level[0];
    const int top = ax.n - 1;
    auto err = [&](float v) {
        const int k = std::max(0, std::min(top, (int)std::floor((v - first) / step + 0.5f)));
        const float d = v - (first + (float)k * step);
        return d * d;
    };
    for (size_t i = 0; i < n; i += stride) { e += err(x[i].real()) + err(x[i].imag()); cnt++; }
    return e / (double)cnt;
}

namespace {
struct BodyTables {
    std::array<int16_t, kSiSymbols> si;
    std::array<int16_t, kDataSymbols> data;
    BodyTables() {
        const auto& pos = siPositions();
        const auto& cm = carrierMap();
        std::array<bool, kBody> isSi{};
        for (int s = 0; s < kSiSymbols; s++) { isSi[(size_t)pos[(size_t)s]] = true; si[(size_t)s] = cm[(size_t)pos[(size_t)s]]; }
        size_t d = 0;
        for (int l = 0; l < kBody; l++) if (!isSi[(size_t)l]) data[d++] = cm[(size_t)l];
    }
};
const BodyTables& bodyTables() { static const BodyTables t; return t; }
}

const int16_t* siBins() { return bodyTables().si.data(); }
const int16_t* dataBins() { return bodyTables().data.data(); }

void splitBody(const cf32* bins, cf32* si, cf32* data) {
    const BodyTables& t = bodyTables();
    for (int i = 0; i < kSiSymbols; i++) si[i] = bins[t.si[(size_t)i]];
    for (int i = 0; i < kDataSymbols; i++) data[i] = bins[t.data[(size_t)i]];
}

} // namespace dect2::dtmb
