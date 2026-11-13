// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Weather picture studio helpers: see wxstudio.h. Clean-room from the TIFF 6.0 and GeoTIFF 1.0 specifications.
#include "dect2/dsp/wxstudio.h"
#include "dect2/common/diag.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {
namespace img {

void histogramOf(const uint8_t* v, size_t n, bool skipZero, std::array<uint32_t, 256>& bins) {
    bins.fill(0);
    for (size_t i = 0; i < n; i++) if (!(skipZero && v[i] == 0)) bins[v[i]]++;
}

std::array<uint8_t, 256> curveLut(const std::vector<CurvePoint>& in) {
    std::vector<CurvePoint> p = in;
    std::sort(p.begin(), p.end(), [](const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });
    std::array<uint8_t, 256> lut;
    if (p.empty()) { for (int i = 0; i < 256; i++) lut[(size_t)i] = (uint8_t)i; return lut; }
    const size_t n = p.size();
    // Fritsch-Carlson slopes: keeps the curve monotone when the points are
    std::vector<double> d(n > 1 ? n - 1 : 0), m(n, 0.0);
    for (size_t i = 0; i + 1 < n; i++) d[i] = p[i + 1].x > p[i].x ? (p[i + 1].y - p[i].y) / (p[i + 1].x - p[i].x) : 0.0;
    if (n > 1) {
        m[0] = d[0]; m[n - 1] = d[n - 2];
        for (size_t i = 1; i + 1 < n; i++) m[i] = d[i - 1] * d[i] <= 0 ? 0.0 : (d[i - 1] + d[i]) / 2;
        for (size_t i = 0; i + 1 < n; i++) {
            if (d[i] == 0) { m[i] = m[i + 1] = 0; continue; }
            const double a = m[i] / d[i], b = m[i + 1] / d[i], s = a * a + b * b;
            if (s > 9) { const double t = 3 / std::sqrt(s); m[i] = t * a * d[i]; m[i + 1] = t * b * d[i]; }
        }
    }
    for (int i = 0; i < 256; i++) {
        const double x = i / 255.0;
        double y;
        if (n == 1 || x <= p[0].x) y = p[0].y;
        else if (x >= p[n - 1].x) y = p[n - 1].y;
        else {
            size_t k = 0;
            while (k + 2 < n && x > p[k + 1].x) k++;
            const double h = p[k + 1].x - p[k].x, t = h > 0 ? (x - p[k].x) / h : 0, t2 = t * t, t3 = t2 * t;
            y = (2 * t3 - 3 * t2 + 1) * p[k].y + (t3 - 2 * t2 + t) * h * m[k] + (-2 * t3 + 3 * t2) * p[k + 1].y + (t3 - t2) * h * m[k + 1];
        }
        lut[(size_t)i] = (uint8_t)std::lround(std::clamp(y, 0.0, 1.0) * 255);
    }
    return lut;
}

const std::vector<CurvePreset>& curvePresets() {
    static const std::vector<CurvePreset> p = {
        {"Linear", {{0, 0}, {1, 1}}},
        {"S-curve (contrast)", {{0, 0}, {0.25, 0.15}, {0.75, 0.85}, {1, 1}}},
        {"Lift shadows", {{0, 0}, {0.2, 0.34}, {0.6, 0.72}, {1, 1}}},
        {"Cloud detail (flat bright end)", {{0, 0}, {0.5, 0.42}, {0.8, 0.78}, {1, 1}}},
        {"Night infrared", {{0, 0}, {0.35, 0.1}, {0.7, 0.6}, {1, 1}}}};
    return p;
}

const std::vector<const char*>& paletteNames() {
    static const std::vector<const char*> n = {"No palette", "Thermal", "Rainbow", "Enhanced infrared", "Sepia"};
    return n;
}

void paletteColour(int index, uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b) {
    const double t = v / 255.0;
    auto B = [](double x) { return (uint8_t)std::lround(std::clamp(x, 0.0, 1.0) * 255); };
    switch (index) {
    case 1: thermalColour(v, r, g, b); break;
    case 2: {   // blue - cyan - green - yellow - red
        const double h = (1 - t) * 240.0 / 60.0;   // hue sector 0 (red) .. 4 (blue)
        const int s = (int)std::floor(h);
        const double f = h - s;
        double R = 0, G = 0, Bl = 0;
        switch (s) { case 0: R = 1; G = f; break; case 1: R = 1 - f; G = 1; break; case 2: G = 1; Bl = f; break; case 3: G = 1 - f; Bl = 1; break; default: Bl = 1; }
        r = B(R); g = B(G); b = B(Bl);
        break;
    }
    case 3: {   // the usual enhanced-IR look: grey for warm land and sea, colour bands for the cold tops
        if (t < 0.5) { r = g = b = B(t * 1.6); }
        else if (t < 0.65) { const double f = (t - 0.5) / 0.15; r = B(0.8 * (1 - f)); g = B(0.8 - 0.2 * f); b = B(0.8 + 0.2 * f); }
        else if (t < 0.8) { const double f = (t - 0.65) / 0.15; r = B(f); g = B(0.6 + 0.4 * f); b = B(1 - f); }
        else if (t < 0.92) { const double f = (t - 0.8) / 0.12; r = 255; g = B(1 - f); b = 0; }
        else { const double f = (t - 0.92) / 0.08; r = 255; g = B(f); b = B(f); }
        break;
    }
    case 4: r = B(t * 1.07); g = B(t * 0.74 + 0.0); b = B(t * 0.43); break;
    default: r = g = b = v;
    }
}

void applyPaletteRgba(uint8_t* rgba, size_t n, int index) {
    if (index <= 0) return;
    uint8_t lut[256][3];
    for (int i = 0; i < 256; i++) paletteColour(index, (uint8_t)i, lut[i][0], lut[i][1], lut[i][2]);
    for (size_t i = 0; i < n; i++) {
        const uint8_t y = (uint8_t)((rgba[i * 4] * 299 + rgba[i * 4 + 1] * 587 + rgba[i * 4 + 2] * 114) / 1000);
        rgba[i * 4] = lut[y][0]; rgba[i * 4 + 1] = lut[y][1]; rgba[i * 4 + 2] = lut[y][2];
    }
}

bool cropMap(const MapImage& in, double west, double north, double east, double south, MapImage& out) {
    if (in.w <= 0 || in.h <= 0 || in.degPerPx <= 0 || in.rgba.size() < (size_t)in.w * in.h * 4) {
        DIAG_DEBUG(787642, "The map crop was given a %d by %d picture with %.6f degrees per pixel and %zu bytes of data, which is not a usable map, so nothing is cropped.", in.w, in.h, in.degPerPx, in.rgba.size());
        return false;
    }
    int x0 = (int)std::floor((std::min(west, east) - in.west) / in.degPerPx), x1 = (int)std::ceil((std::max(west, east) - in.west) / in.degPerPx);
    int y0 = (int)std::floor((in.north - std::max(north, south)) / in.degPerPx), y1 = (int)std::ceil((in.north - std::min(north, south)) / in.degPerPx);
    x0 = std::clamp(x0, 0, in.w); x1 = std::clamp(x1, 0, in.w); y0 = std::clamp(y0, 0, in.h); y1 = std::clamp(y1, 0, in.h);
    if (x1 - x0 < 1 || y1 - y0 < 1) {
        DIAG_DEBUG(917801, "The map crop area lies outside the picture, which covers %d by %d pixels, so nothing is cropped.", in.w, in.h);
        return false;
    }
    MapImage o;
    o.w = x1 - x0; o.h = y1 - y0; o.degPerPx = in.degPerPx;
    o.west = in.west + x0 * in.degPerPx; o.north = in.north - y0 * in.degPerPx;
    o.rgba.resize((size_t)o.w * o.h * 4);
    for (int y = 0; y < o.h; y++) std::memcpy(&o.rgba[(size_t)y * o.w * 4], &in.rgba[((size_t)(y + y0) * in.w + x0) * 4], (size_t)o.w * 4);
    out = std::move(o);
    return true;
}

bool mosaicMaps(const std::vector<MapImage>& parts, int maxDim, MapImage& out) {
    double west = 1e9, north = -1e9, east = -1e9, south = 1e9, deg = 1e9;
    size_t used = 0;
    for (const MapImage& m : parts) {
        if (m.w <= 0 || m.h <= 0 || m.degPerPx <= 0 || m.rgba.size() < (size_t)m.w * m.h * 4) continue;
        used++;
        west = std::min(west, m.west); north = std::max(north, m.north);
        east = std::max(east, m.west + m.w * m.degPerPx); south = std::min(south, m.north - m.h * m.degPerPx);
        deg = std::min(deg, m.degPerPx);
    }
    if (!used || maxDim < 16) return false;
    deg = std::max(deg, std::max(east - west, north - south) / maxDim);
    MapImage o;
    o.w = std::max(1, (int)std::ceil((east - west) / deg)); o.h = std::max(1, (int)std::ceil((north - south) / deg));
    o.west = west; o.north = north; o.degPerPx = deg;
    const size_t np = (size_t)o.w * o.h;
    std::vector<uint32_t> sum(np * 3, 0);
    std::vector<uint16_t> cnt(np, 0);
    for (const MapImage& m : parts) {
        if (m.w <= 0 || m.h <= 0 || m.degPerPx <= 0 || m.rgba.size() < (size_t)m.w * m.h * 4) continue;
        const int dx0 = std::max(0, (int)std::floor((m.west - west) / deg)), dx1 = std::min(o.w, (int)std::ceil((m.west + m.w * m.degPerPx - west) / deg));
        const int dy0 = std::max(0, (int)std::floor((north - m.north) / deg)), dy1 = std::min(o.h, (int)std::ceil((north - (m.north - m.h * m.degPerPx)) / deg));
        for (int y = dy0; y < dy1; y++) {
            const int sy = (int)std::floor((m.north - (north - (y + 0.5) * deg)) / m.degPerPx);
            if (sy < 0 || sy >= m.h) continue;
            for (int x = dx0; x < dx1; x++) {
                const int sx = (int)std::floor((west + (x + 0.5) * deg - m.west) / m.degPerPx);
                if (sx < 0 || sx >= m.w) continue;
                const uint8_t* s = &m.rgba[((size_t)sy * m.w + sx) * 4];
                if (!s[3]) continue;
                const size_t d = (size_t)y * o.w + x;
                sum[d * 3] += s[0]; sum[d * 3 + 1] += s[1]; sum[d * 3 + 2] += s[2];
                if (cnt[d] < 65535) cnt[d]++;
            }
        }
    }
    o.rgba.assign(np * 4, 0);
    for (size_t i = 0; i < np; i++) {
        if (!cnt[i]) continue;
        o.rgba[i * 4] = (uint8_t)(sum[i * 3] / cnt[i]); o.rgba[i * 4 + 1] = (uint8_t)(sum[i * 3 + 1] / cnt[i]); o.rgba[i * 4 + 2] = (uint8_t)(sum[i * 3 + 2] / cnt[i]); o.rgba[i * 4 + 3] = 255;
    }
    out = std::move(o);
    return true;
}

namespace {
void p16(std::vector<uint8_t>& b, uint32_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
void p32(std::vector<uint8_t>& b, uint32_t v) { p16(b, v & 0xFFFF); p16(b, v >> 16); }
void pd(std::vector<uint8_t>& b, double d) { uint64_t u; std::memcpy(&u, &d, 8); p32(b, (uint32_t)u); p32(b, (uint32_t)(u >> 32)); }
}  // namespace

std::vector<uint8_t> geoTiff(const MapImage& m) {
    std::vector<uint8_t> out;
    if (m.w <= 0 || m.h <= 0 || m.degPerPx <= 0 || m.rgba.size() < (size_t)m.w * m.h * 4) return out;
    const uint32_t nTags = 14, ifdOff = 8, ifdSize = 2 + nTags * 12 + 4;
    uint32_t off = ifdOff + ifdSize;
    const uint32_t bitsOff = off; off += 8;       // 4 x SHORT
    const uint32_t scaleOff = off; off += 24;     // 3 x DOUBLE
    const uint32_t tieOff = off; off += 48;       // 6 x DOUBLE
    const uint32_t keysOff = off; off += 32;      // header + 3 keys, 4 SHORTs each
    const uint32_t dataOff = off;
    const uint32_t dataLen = (uint32_t)m.w * (uint32_t)m.h * 4;
    out.reserve(dataOff + dataLen);
    out.push_back('I'); out.push_back('I'); p16(out, 42); p32(out, ifdOff);
    p16(out, nTags);
    auto tag = [&](uint16_t id, uint16_t type, uint32_t count, uint32_t value) { p16(out, id); p16(out, type); p32(out, count); p32(out, value); };
    const uint16_t SHORT = 3, LONG = 4, DOUBLE = 12;
    tag(256, LONG, 1, (uint32_t)m.w);
    tag(257, LONG, 1, (uint32_t)m.h);
    tag(258, SHORT, 4, bitsOff);
    tag(259, SHORT, 1, 1);                        // no compression
    tag(262, SHORT, 1, 2);                        // RGB
    tag(273, LONG, 1, dataOff);
    tag(277, SHORT, 1, 4);
    tag(278, LONG, 1, (uint32_t)m.h);
    tag(279, LONG, 1, dataLen);
    tag(284, SHORT, 1, 1);
    tag(338, SHORT, 1, 2);                        // the 4th sample is unassociated alpha
    tag(33550, DOUBLE, 3, scaleOff);
    tag(33922, DOUBLE, 6, tieOff);
    tag(34735, SHORT, 16, keysOff);
    p32(out, 0);                                  // no further IFD
    for (int i = 0; i < 4; i++) p16(out, 8);
    pd(out, m.degPerPx); pd(out, m.degPerPx); pd(out, 0);
    pd(out, 0); pd(out, 0); pd(out, 0); pd(out, m.west); pd(out, m.north); pd(out, 0);
    const uint16_t keys[16] = {1, 1, 0, 3,        // version 1.1.0, three keys
                               1024, 0, 1, 2,     // GTModelTypeGeoKey = ModelTypeGeographic
                               1025, 0, 1, 1,     // GTRasterTypeGeoKey = RasterPixelIsArea
                               2048, 0, 1, 4326}; // GeographicTypeGeoKey = WGS 84
    for (uint16_t k : keys) p16(out, k);
    out.insert(out.end(), m.rgba.begin(), m.rgba.begin() + dataLen);
    return out;
}

}  // namespace img
}  // namespace dect2
