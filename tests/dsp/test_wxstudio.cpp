// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Weather studio: histogram, tone curves, palettes, vectors on a map, crop, mosaic of two synthetic passes, and the GeoTIFF read back.
#include "dect2/dsp/wxstudio.h"
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace dect2::img;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static MapImage blank(int w, int h, double west, double north, double deg, uint8_t v, bool opaque = true) {
    MapImage m; m.w = w; m.h = h; m.west = west; m.north = north; m.degPerPx = deg;
    m.rgba.assign((size_t)w * h * 4, 0);
    for (size_t i = 0; i < (size_t)w * h; i++) { m.rgba[i * 4] = m.rgba[i * 4 + 1] = m.rgba[i * 4 + 2] = v; m.rgba[i * 4 + 3] = opaque ? 255 : 0; }
    return m;
}
static uint32_t rd(const std::vector<uint8_t>& b, size_t o, int n) { uint32_t v = 0; for (int i = n - 1; i >= 0; i--) v = v << 8 | b[o + (size_t)i]; return v; }
static double rdd(const std::vector<uint8_t>& b, size_t o) { uint64_t u = (uint64_t)rd(b, o + 4, 4) << 32 | rd(b, o, 4); double d; std::memcpy(&d, &u, 8); return d; }

int main() {
    std::vector<uint8_t> v = {0, 0, 5, 5, 5, 200};
    std::array<uint32_t, 256> h;
    histogramOf(v.data(), v.size(), false, h);
    CHECK(h[0] == 2 && h[5] == 3 && h[200] == 1, "histogram");
    histogramOf(v.data(), v.size(), true, h);
    CHECK(h[0] == 0 && h[5] == 3, "histogram skipZero");

    for (const auto& p : curvePresets()) {
        auto l = curveLut(p.pts);
        bool mono = true;
        for (int i = 1; i < 256; i++) if (l[(size_t)i] < l[(size_t)i - 1]) mono = false;
        CHECK(mono && l[0] == 0 && l[255] == 255, "curve %s monotone and spans", p.name);
    }
    auto lin = curveLut(curvePresets()[0].pts);
    CHECK(std::abs((int)lin[100] - 100) <= 1, "linear curve %d", lin[100]);
    auto sc = curveLut(curvePresets()[1].pts);
    CHECK(sc[64] < 64 && sc[192] > 192, "S-curve darkens shadows, brightens highlights: %d %d", sc[64], sc[192]);
    auto ov = curveLut({{0, 0}, {0.5, 1}, {0.6, 1}, {1, 1}});
    CHECK(ov[255] == 255 && ov[128] <= 255, "no overshoot");

    uint8_t r, g, b;
    for (size_t pi = 1; pi < paletteNames().size(); pi++) {
        paletteColour((int)pi, 0, r, g, b);
        const uint8_t r0 = r, g0 = g, b0 = b;
        paletteColour((int)pi, 255, r, g, b);
        CHECK(r0 != r || g0 != g || b0 != b, "palette %zu ends differ", pi);
    }
    uint8_t px[4] = {10, 10, 10, 255};
    applyPaletteRgba(px, 1, 0);
    CHECK(px[0] == 10, "no palette keeps");
    applyPaletteRgba(px, 1, 2);
    CHECK(px[3] == 255 && px[2] > px[0], "rainbow: dark is blue");

    // vectors: a 1 degree map around the Mediterranean contains coast pixels; one in the middle of the Pacific none
    MapImage med = blank(120, 60, -5, 45, 0.25, 0);
    drawVectors(med, kVecCoast, 0xFFFFFFFFu);
    size_t lit = 0;
    for (size_t i = 0; i < (size_t)med.w * med.h; i++) if (med.rgba[i * 4]) lit++;
    CHECK(lit > 200, "coast pixels in the western Mediterranean: %zu", lit);
    MapImage sea = blank(100, 100, -150, 5, 0.1, 0);
    drawVectors(sea, kVecCoast | kVecBorder, 0xFFFFFFFFu);
    lit = 0;
    for (size_t i = 0; i < (size_t)sea.w * sea.h; i++) if (sea.rgba[i * 4]) lit++;
    CHECK(lit == 0, "no coast in open Pacific: %zu", lit);
    // Strait of Gibraltar to Africa: the point (-5.9, 36) is on a coast, so a map around it is lit within a pixel
    MapImage gib = blank(40, 40, -7, 37, 0.05, 0);
    drawVectors(gib, kVecCoast, 0xFFFFFFFFu);
    const int gx = (int)std::floor((-5.9 + 7) / 0.05), gy = (int)std::floor((37 - 36) / 0.05);
    bool nearBy = false;
    for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) if (gib.rgba[((size_t)(gy + dy) * gib.w + gx + dx) * 4]) nearBy = true;
    CHECK(nearBy, "Gibraltar coast");
    // a map crossing the antimeridian still draws (no streak across it)
    MapImage am = blank(200, 50, 170, 70, 0.1, 0);
    drawVectors(am, kVecCoast, 0xFFFFFFFFu);
    for (int y = 0; y < am.h; y++) {   // a full row of lit pixels would be a wrap streak
        int c = 0;
        for (int x = 0; x < am.w; x++) if (am.rgba[((size_t)y * am.w + x) * 4]) c++;
        CHECK(c < am.w / 2, "row %d streak %d", y, c);
    }

    // crop keeps the geography
    MapImage big = blank(100, 80, 10, 60, 0.5, 0);
    for (int y = 0; y < 80; y++) for (int x = 0; x < 100; x++) big.rgba[((size_t)y * 100 + x) * 4] = (uint8_t)x;
    MapImage c;
    CHECK(cropMap(big, 20, 50, 30, 40, c), "crop ok");
    CHECK(c.w == 20 && c.h == 20 && std::abs(c.west - 20) < 1e-9 && std::abs(c.north - 50) < 1e-9 && c.rgba[0] == 20, "crop %d x %d west %.2f north %.2f first %d", c.w, c.h, c.west, c.north, c.rgba[0]);
    CHECK(!cropMap(big, 200, 50, 210, 40, c), "crop outside fails");

    // mosaic of two passes side by side with a one degree overlap; the overlap is the mean
    std::vector<MapImage> parts = {blank(20, 20, 0, 10, 0.5, 100), blank(20, 20, 9, 10, 0.5, 200)};
    MapImage mo;
    CHECK(mosaicMaps(parts, 1000, mo), "mosaic ok");
    CHECK(mo.w == 38 && mo.h == 20 && std::abs(mo.west) < 1e-9 && std::abs(mo.north - 10) < 1e-9, "mosaic size %d x %d", mo.w, mo.h);
    CHECK(mo.rgba[(size_t)5 * mo.w * 4] == 100 && mo.rgba[((size_t)5 * mo.w + 37) * 4] == 200, "mosaic ends");
    CHECK(mo.rgba[((size_t)5 * mo.w + 18) * 4 + 0] == 150 || mo.rgba[((size_t)5 * mo.w + 19) * 4] == 150, "mosaic overlap mean %d", mo.rgba[((size_t)5 * mo.w + 18) * 4]);
    // an uncovered gap stays transparent
    parts = {blank(10, 10, 0, 10, 0.5, 100), blank(10, 10, 5, 10, 0.5, 100)};
    CHECK(mosaicMaps(parts, 1000, mo) && mo.w == 20 && mo.rgba[(size_t)5 * mo.w * 4 + 3] == 255, "mosaic adjacent");
    parts = {blank(10, 10, 0, 10, 0.5, 100), blank(10, 10, 20, 10, 0.5, 100)};
    CHECK(mosaicMaps(parts, 1000, mo) && mo.rgba[((size_t)5 * mo.w + 25) * 4 + 3] == 0, "mosaic gap transparent");
    CHECK(mosaicMaps(parts, 20, mo) && mo.w <= 21 && std::abs(mo.degPerPx - 25.0 / 20) < 1e-9 * 100, "mosaic limited to maxDim: %d, %.3f", mo.w, mo.degPerPx);
    CHECK(!mosaicMaps({}, 100, mo), "empty mosaic fails");

    // GeoTIFF read back: header, size, strip, geo tags
    MapImage gm = blank(7, 5, -12.5, 48.25, 0.125, 0);
    for (size_t i = 0; i < 35; i++) { gm.rgba[i * 4] = (uint8_t)i; gm.rgba[i * 4 + 1] = (uint8_t)(i * 2); gm.rgba[i * 4 + 2] = 7; }
    const auto t = geoTiff(gm);
    CHECK(t.size() > 140 && t[0] == 'I' && t[1] == 'I' && rd(t, 2, 2) == 42, "tiff header");
    const uint32_t ifd = rd(t, 4, 4), n = rd(t, ifd, 2);
    uint32_t w = 0, hh = 0, strip = 0, cnt = 0, tieOff = 0, scaleOff = 0, keysOff = 0, spp = 0;
    uint32_t prev = 0;
    for (uint32_t i = 0; i < n; i++) {
        const size_t e = ifd + 2 + i * 12;
        const uint32_t id = rd(t, e, 2), val = rd(t, e + 8, 4);
        CHECK(id > prev, "tags ascending");
        prev = id;
        if (id == 256) w = val; else if (id == 257) hh = val; else if (id == 273) strip = val; else if (id == 279) cnt = val;
        else if (id == 277) spp = val; else if (id == 33922) tieOff = val; else if (id == 33550) scaleOff = val; else if (id == 34735) keysOff = val;
    }
    CHECK(w == 7 && hh == 5 && spp == 4 && cnt == 7 * 5 * 4 && strip + cnt == t.size(), "tiff geometry %u %u %u", w, hh, strip);
    CHECK(std::memcmp(&t[strip], gm.rgba.data(), cnt) == 0, "tiff pixels");
    CHECK(rdd(t, scaleOff) == 0.125 && rdd(t, scaleOff + 8) == 0.125 && rdd(t, tieOff + 24) == -12.5 && rdd(t, tieOff + 32) == 48.25, "geo tags");
    CHECK(rd(t, keysOff + 4 * 2, 2) == 1024 && rd(t, keysOff + 15 * 2, 2) == 4326, "geokeys");
    CHECK(geoTiff(MapImage()).empty(), "empty map gives no tiff");

    printf(fails ? "wxstudio: %d FAILED\n" : "wxstudio: all passed\n", fails);
    return fails ? 1 : 0;
}
