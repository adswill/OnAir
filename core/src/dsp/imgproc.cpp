// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/dsp/imgproc.h"
#include "dect2/common/diag.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "dect2/common/mathconst.h"

namespace dect2 {
namespace img {

using dect2::math::kPi;
static inline double rad(double d) { return d * kPi / 180.0; }
static inline double deg(double r) { return r * 180.0 / kPi; }

void percentileRange(const uint8_t* v, size_t n, double loPct, double hiPct, bool skipZero, int& lo, int& hi) {
    size_t hist[256] = {};
    size_t tot = 0;
    for (size_t i = 0; i < n; i++) { if (skipZero && v[i] == 0) continue; hist[v[i]]++; tot++; }
    lo = 0; hi = 255;
    if (!tot) return;
    const double a = loPct / 100.0 * (double)tot, b = hiPct / 100.0 * (double)tot;
    double acc = 0;
    bool gotLo = false;
    for (int i = 0; i < 256; i++) {
        acc += (double)hist[i];
        if (!gotLo && acc > a) { lo = i; gotLo = true; }
        if (acc >= b) { hi = i; break; }
    }
    if (hi <= lo) hi = std::min(255, lo + 1);
}

std::array<uint8_t, 256> stretchLut(int lo, int hi) {
    std::array<uint8_t, 256> l;
    if (hi <= lo) hi = lo + 1;
    for (int i = 0; i < 256; i++) l[(size_t)i] = (uint8_t)std::clamp((int)std::lround((double)(i - lo) * 255.0 / (double)(hi - lo)), 0, 255);
    return l;
}

std::array<uint8_t, 256> gammaLut(double gamma) {
    std::array<uint8_t, 256> l;
    const double e = 1.0 / std::max(0.01, gamma);
    for (int i = 0; i < 256; i++) l[(size_t)i] = (uint8_t)std::clamp((int)std::lround(255.0 * std::pow(i / 255.0, e)), 0, 255);
    return l;
}

std::array<uint8_t, 256> equaliseLut(const uint8_t* v, size_t n, bool skipZero) {
    size_t hist[256] = {};
    size_t tot = 0;
    for (size_t i = 0; i < n; i++) { if (skipZero && v[i] == 0) continue; hist[v[i]]++; tot++; }
    std::array<uint8_t, 256> l;
    for (int i = 0; i < 256; i++) l[(size_t)i] = (uint8_t)i;
    if (!tot) return l;
    size_t cdf = 0, cdfMin = 0;
    for (int i = 0; i < 256; i++) if (hist[i]) { cdfMin = hist[i]; break; }
    for (int i = 0; i < 256; i++) {
        cdf += hist[i];
        if (tot > cdfMin) l[(size_t)i] = (uint8_t)std::clamp((int)std::lround((double)(cdf > cdfMin ? cdf - cdfMin : 0) * 255.0 / (double)(tot - cdfMin)), 0, 255);
    }
    if (skipZero) l[0] = 0;
    return l;
}

std::array<uint8_t, 256> invertLut() {
    std::array<uint8_t, 256> l;
    for (int i = 0; i < 256; i++) l[(size_t)i] = (uint8_t)(255 - i);
    return l;
}

std::array<uint8_t, 256> composeLut(const std::array<uint8_t, 256>& first, const std::array<uint8_t, 256>& then) {
    std::array<uint8_t, 256> l;
    for (int i = 0; i < 256; i++) l[(size_t)i] = then[first[(size_t)i]];
    return l;
}

void thermalColour(uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b) {
    static const double stops[7][3] = {{0, 0, 0}, {0, 0, 160}, {0, 200, 220}, {0, 190, 40}, {240, 230, 0}, {230, 40, 0}, {255, 255, 255}};
    const double x = v / 255.0 * 6.0;
    const int i = std::min(5, (int)x);
    const double f = x - i;
    uint8_t* o[3] = {&r, &g, &b};
    for (int c = 0; c < 3; c++) *o[c] = (uint8_t)std::lround(stops[i][c] + (stops[i + 1][c] - stops[i][c]) * f);
}

const std::vector<Preset>& falseColourPresets() {
    static const std::vector<Preset> p = {
        {"As received", -1, -1, -1, false}, {"123 (visible)", 0, 1, 2, false}, {"221 (vegetation)", 1, 1, 0, false},
        {"321", 2, 1, 0, false}, {"125 (infrared as blue)", 0, 1, 4, false}, {"Infrared, thermal palette (ch 5)", -1, -1, 4, true},
        {"Infrared, thermal palette (ch 4)", -1, -1, 3, true}, {"Infrared, thermal palette (ch 6)", -1, -1, 5, true}};
    return p;
}

void composeRgba(const uint8_t* pr, const uint8_t* pg, const uint8_t* pb, size_t n, bool thermal, uint8_t* rgba) {
    for (size_t i = 0; i < n; i++) {
        if (thermal) thermalColour(pb ? pb[i] : 0, rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]);
        else { rgba[i * 4] = pr ? pr[i] : 0; rgba[i * 4 + 1] = pg ? pg[i] : 0; rgba[i * 4 + 2] = pb ? pb[i] : 0; }
        rgba[i * 4 + 3] = 255;
    }
}

void applyLutRgba(uint8_t* rgba, size_t n, const std::array<uint8_t, 256>& lut) {
    for (size_t i = 0; i < n; i++) { rgba[i * 4] = lut[rgba[i * 4]]; rgba[i * 4 + 1] = lut[rgba[i * 4 + 1]]; rgba[i * 4 + 2] = lut[rgba[i * 4 + 2]]; }
}

std::vector<uint8_t> lumaOf(const uint8_t* rgba, size_t n) {
    std::vector<uint8_t> y(n);
    for (size_t i = 0; i < n; i++) y[i] = (uint8_t)((rgba[i * 4] * 299 + rgba[i * 4 + 1] * 587 + rgba[i * 4 + 2] * 114) / 1000);
    return y;
}

// ---------------------------------------------------------------- geometry

TrackPoint destination(const TrackPoint& p, double brg, double d) {
    const double dr = d / kEarthKm, b = rad(brg), la = rad(p.lat), lo = rad(p.lon);
    const double la2 = std::asin(std::clamp(std::sin(la) * std::cos(dr) + std::cos(la) * std::sin(dr) * std::cos(b), -1.0, 1.0));
    const double lo2 = lo + std::atan2(std::sin(b) * std::sin(dr) * std::cos(la), std::cos(dr) - std::sin(la) * std::sin(la2));
    TrackPoint o;
    o.lat = deg(la2);
    o.lon = deg(std::remainder(lo2, 2 * kPi));
    return o;
}

double distanceKm(const TrackPoint& a, const TrackPoint& b) {
    const double la1 = rad(a.lat), la2 = rad(b.lat), dl = rad(b.lon - a.lon);
    const double c = std::clamp(std::sin(la1) * std::sin(la2) + std::cos(la1) * std::cos(la2) * std::cos(dl), -1.0, 1.0);
    return kEarthKm * std::acos(c);
}

double bearingDeg(const TrackPoint& a, const TrackPoint& b) {
    const double la1 = rad(a.lat), la2 = rad(b.lat), dl = rad(b.lon - a.lon);
    return deg(std::atan2(std::sin(dl) * std::cos(la2), std::cos(la1) * std::sin(la2) - std::sin(la1) * std::cos(la2) * std::cos(dl)));
}

// the nadir point and the direction of flight at a (fractional) row; outside the rows the last segment's great circle is extended
static void nadirAt(const SwathGeom& g, double row, TrackPoint& p, double& brg, double& step) {
    const int n = (int)g.track.size();
    const int i = std::clamp((int)std::floor(row), 0, std::max(0, n - 2));
    const double f = row - i;
    const TrackPoint& a = g.track[(size_t)i];
    const TrackPoint& b = g.track[(size_t)i + 1];
    brg = bearingDeg(a, b);
    step = distanceKm(a, b);
    p = destination(a, brg, step * f);
}

TrackPoint pixelLatLon(const SwathGeom& g, double row, double col) {
    TrackPoint p;
    if (g.track.size() < 2) return g.track.empty() ? p : g.track[0];
    double brg, step;
    nadirAt(g, row, p, brg, step);
    double off = ((col + 0.5) / g.cols - 0.5) * g.swathKm;   // + to the right of the flight direction at column 0 on the right
    if (!g.colZeroRight) off = -off;
    return destination(p, brg + 90.0, -off);
}

bool latLonPixel(const SwathGeom& g, const TrackPoint& q, double& row, double& col) {
    const int n = (int)g.track.size();
    if (n < 2) return false;
    int best = 0;
    double bd = 1e18;
    for (int i = 0; i < n; i++) {
        const double d = distanceKm(g.track[(size_t)i], q);
        if (d < bd) { bd = d; best = i; }
    }
    double r = best;
    for (int it = 0; it < 5; it++) {   // move along the track by the part of the offset that lies along it
        TrackPoint p;
        double brg, step;
        nadirAt(g, r, p, brg, step);
        if (step <= 1e-9) break;
        r += distanceKm(p, q) * std::cos(rad(bearingDeg(p, q) - brg)) / step;
    }
    TrackPoint p;
    double brg, step;
    nadirAt(g, r, p, brg, step);
    // signed cross-track distance, positive to the right of the direction of flight
    const double right = distanceKm(p, q) * std::sin(rad(bearingDeg(p, q) - brg));
    double off = -right;
    if (!g.colZeroRight) off = -off;
    const double c = (off / g.swathKm + 0.5) * g.cols - 0.5;
    row = r; col = c;
    return r >= -0.5 && r <= n - 0.5 && c >= -0.5 && c <= g.cols - 0.5;
}

// ---------------------------------------------------------------- projection

bool projectSwath(const uint8_t* rgba, int cols, int lines, int lpt, const SwathGeom& g, int maxDim, MapImage& out) {
    if (!rgba || cols <= 0 || lines <= 0 || lpt <= 0 || g.track.size() < 2 || maxDim < 16) {
        DIAG_DEBUG(678991, "The swath projection was given a %d by %d picture, %d lines per track point, %d track points and a limit of %d pixels, which cannot be projected, so no map is made.", cols, lines, lpt, (int)g.track.size(), maxDim);
        return false;
    }
    const int rows = (int)g.track.size();
    const double lon0 = g.track[0].lon;
    auto unwrap = [&](double lon) { return lon0 + std::remainder(lon - lon0, 360.0); };   // the pass may cross the antimeridian
    double west = 1e9, east = -1e9, south = 1e9, north = -1e9;
    const int cs[3] = {0, cols / 2, cols - 1};
    for (int r = 0; r < rows; r++)
        for (int c : cs) {
            const TrackPoint p = pixelLatLon(g, r, c);
            const double lo = unwrap(p.lon);
            west = std::min(west, lo); east = std::max(east, lo); south = std::min(south, p.lat); north = std::max(north, p.lat);
        }
    west -= 0.2; east += 0.2; south = std::max(-90.0, south - 0.2); north = std::min(90.0, north + 0.2);
    const double span = std::max(east - west, north - south);
    const double srcDeg = g.swathKm / cols / 111.0;   // one degree is about 111 km
    const double d1 = std::max(span / maxDim, srcDeg);
    out.w = std::max(1, (int)std::ceil((east - west) / d1));
    out.h = std::max(1, (int)std::ceil((north - south) / d1));
    out.west = west; out.north = north; out.degPerPx = d1;
    out.rgba.assign((size_t)out.w * out.h * 4, 0);
    std::vector<float> acc((size_t)out.w * out.h * 3, 0.f);
    std::vector<uint16_t> cnt((size_t)out.w * out.h, 0);
    const int maxLines = std::min(lines, rows * lpt);
    for (int y = 0; y < maxLines; y++) {
        const double row = (double)y / lpt;
        for (int x = 0; x < cols; x++) {
            const uint8_t* s = rgba + ((size_t)y * cols + x) * 4;
            if (s[3] == 0) continue;
            const TrackPoint p = pixelLatLon(g, row, x);
            const int px = (int)std::floor((unwrap(p.lon) - west) / d1), py = (int)std::floor((north - p.lat) / d1);
            if (px < 0 || py < 0 || px >= out.w || py >= out.h) continue;
            const size_t k = (size_t)py * out.w + px;
            acc[k * 3] += s[0]; acc[k * 3 + 1] += s[1]; acc[k * 3 + 2] += s[2]; cnt[k]++;
        }
    }
    for (size_t k = 0; k < cnt.size(); k++)
        if (cnt[k]) {
            out.rgba[k * 4] = (uint8_t)(acc[k * 3] / cnt[k]); out.rgba[k * 4 + 1] = (uint8_t)(acc[k * 3 + 1] / cnt[k]);
            out.rgba[k * 4 + 2] = (uint8_t)(acc[k * 3 + 2] / cnt[k]); out.rgba[k * 4 + 3] = 255;
        }
    // close the small holes the forward mapping leaves: an empty pixel takes the mean of its covered neighbours when at least 5 of 8 are there
    for (int pass = 0; pass < 2; pass++) {
        const std::vector<uint8_t> copy = out.rgba;
        for (int y = 1; y < out.h - 1; y++)
            for (int x = 1; x < out.w - 1; x++) {
                const size_t k = (size_t)y * out.w + x;
                if (copy[k * 4 + 3]) continue;
                int n = 0, s[3] = {0, 0, 0};
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        const size_t j = ((size_t)(y + dy) * out.w + (x + dx)) * 4;
                        if (copy[j + 3]) { n++; s[0] += copy[j]; s[1] += copy[j + 1]; s[2] += copy[j + 2]; }
                    }
                if (n >= 5) { out.rgba[k * 4] = (uint8_t)(s[0] / n); out.rgba[k * 4 + 1] = (uint8_t)(s[1] / n); out.rgba[k * 4 + 2] = (uint8_t)(s[2] / n); out.rgba[k * 4 + 3] = 255; }
            }
    }
    return true;
}

void drawGraticule(MapImage& m, double step, uint32_t col) {
    if (m.w <= 0 || m.degPerPx <= 0 || step <= 0) return;
    const uint8_t c[4] = {(uint8_t)(col & 255), (uint8_t)(col >> 8 & 255), (uint8_t)(col >> 16 & 255), (uint8_t)(col >> 24 & 255)};
    auto put = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= m.w || y >= m.h) return;
        uint8_t* p = &m.rgba[((size_t)y * m.w + x) * 4];
        p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; p[3] = c[3] ? c[3] : 255;
    };
    const double east = m.west + m.w * m.degPerPx, south = m.north - m.h * m.degPerPx;
    for (double lon = std::ceil(m.west / step) * step; lon <= east; lon += step) {
        const int x = (int)std::floor((lon - m.west) / m.degPerPx);
        for (int y = 0; y < m.h; y++) put(x, y);
    }
    for (double lat = std::ceil(south / step) * step; lat <= m.north; lat += step) {
        const int y = (int)std::floor((m.north - lat) / m.degPerPx);
        for (int x = 0; x < m.w; x++) put(x, y);
    }
}

std::string worldFile(const MapImage& m) {
    char b[256];
    snprintf(b, sizeof b, "%.10f\n0.0\n0.0\n%.10f\n%.10f\n%.10f\n", m.degPerPx, -m.degPerPx, m.west + m.degPerPx / 2, m.north - m.degPerPx / 2);
    return b;
}

}  // namespace img
}  // namespace dect2
