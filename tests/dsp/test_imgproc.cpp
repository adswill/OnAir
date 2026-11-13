// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Picture enhancement and the swath projection: LUT properties, percentile range, and lat/lon <-> pixel round trips on a synthetic pass.
#include "dect2/dsp/imgproc.h"
#include <cmath>
#include <cstdio>
using namespace dect2::img;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main() {
    auto s = stretchLut(50, 150);
    CHECK(s[50] == 0 && s[150] == 255 && s[100] == 128 && s[0] == 0 && s[255] == 255, "stretch");
    auto g1 = gammaLut(1.0), g2 = gammaLut(2.0);
    CHECK(g1[77] == 77 && g2[0] == 0 && g2[255] == 255 && g2[64] > 64, "gamma");
    auto inv = invertLut();
    CHECK(inv[0] == 255 && composeLut(inv, inv)[91] == 91, "invert");
    std::vector<uint8_t> ramp;
    for (int r = 0; r < 10; r++) for (int i = 0; i < 256; i++) ramp.push_back((uint8_t)i);
    int lo, hi;
    percentileRange(ramp.data(), ramp.size(), 2, 98, false, lo, hi);
    CHECK(lo >= 4 && lo <= 6 && hi >= 249 && hi <= 252, "percentile %d %d", lo, hi);
    std::vector<uint8_t> z(1000, 0);
    for (int i = 0; i < 100; i++) z[(size_t)i] = (uint8_t)(100 + i);
    percentileRange(z.data(), z.size(), 0, 100, true, lo, hi);
    CHECK(lo == 100 && hi == 199, "skipZero %d %d", lo, hi);
    std::vector<uint8_t> two(100, 100);
    for (int i = 0; i < 50; i++) two[(size_t)i] = 120;
    auto eq = equaliseLut(two.data(), two.size(), false);
    CHECK(eq[100] == 0 && eq[120] == 255, "equalise %d %d", eq[100], eq[120]);
    auto eqr = equaliseLut(ramp.data(), ramp.size(), false);
    bool mono = true;
    for (int i = 1; i < 256; i++) if (eqr[(size_t)i] < eqr[(size_t)i - 1]) mono = false;
    CHECK(mono && eqr[255] == 255 && std::abs(eqr[128] - 128) <= 2, "equalise ramp");
    uint8_t r, g, b;
    thermalColour(0, r, g, b); CHECK(r == 0 && g == 0 && b == 0, "thermal 0");
    thermalColour(255, r, g, b); CHECK(r == 255 && g == 255 && b == 255, "thermal 255");
    uint8_t pr[2] = {10, 20}, pg[2] = {30, 40}, pb[2] = {50, 60}, o[8];
    composeRgba(pr, pg, pb, 2, false, o);
    CHECK(o[0] == 10 && o[1] == 30 && o[2] == 50 && o[3] == 255 && o[4] == 20, "compose");
    CHECK(falseColourPresets().size() >= 5, "presets");

    TrackPoint a{10, 20}, b2 = destination(a, 90, 500);
    CHECK(std::abs(distanceKm(a, b2) - 500) < 0.01, "dist");
    CHECK(std::abs(bearingDeg(a, destination(a, 37, 800)) - 37) < 0.01, "bearing");

    // a synthetic southbound pass: 300 track points about 8.7 km apart
    SwathGeom geo;
    geo.cols = 1568; geo.swathKm = 2800;
    TrackPoint p{60, 10};
    for (int i = 0; i < 300; i++) { geo.track.push_back(p); p = destination(p, 190 + i * 0.01, 8.7); }
    int bad = 0;
    double worst = 0;
    for (int row : {0, 5, 150, 298}) for (int col : {0, 100, 784, 1500, 1567}) {
        const TrackPoint q = pixelLatLon(geo, row + 0.3, col);
        double rr, cc;
        const bool ok = latLonPixel(geo, q, rr, cc);
        const double e = std::hypot(rr - (row + 0.3), cc - col);
        worst = std::fmax(worst, e);
        if (!ok || e > 1.0) bad++;
    }
    CHECK(bad == 0, "round trip: %d bad, worst error %.3f px", bad, worst);
    const TrackPoint c = pixelLatLon(geo, 100, 783.5), e0 = pixelLatLon(geo, 100, 0);
    CHECK(distanceKm(c, geo.track[100]) < 0.5, "centre off track");
    CHECK(std::abs(distanceKm(e0, geo.track[100]) - 1400 * (1 - 0.5 / 784)) < 15, "edge %.1f km", distanceKm(e0, geo.track[100]));
    CHECK(e0.lon < c.lon, "col 0 should be west for a southbound pass");
    double rr, cc;
    CHECK(!latLonPixel(geo, destination(geo.track[100], 100, 2000), rr, cc), "outside swath accepted");

    const int lines = 300 * 8;
    std::vector<uint8_t> src((size_t)lines * geo.cols * 4, 255);
    for (int y = 0; y < lines; y++) for (int x = 0; x < geo.cols; x++) {
        uint8_t* q = &src[((size_t)y * geo.cols + x) * 4];
        q[0] = std::abs(x - 784) < 20 ? 255 : 0; q[1] = 0; q[2] = 0;
    }
    MapImage m;
    CHECK(projectSwath(src.data(), geo.cols, lines, 8, geo, 800, m) && m.w > 10 && m.h > 10, "project");
    const TrackPoint mid = geo.track[150];
    const int px = (int)((mid.lon - m.west) / m.degPerPx), py = (int)((m.north - mid.lat) / m.degPerPx);
    const uint8_t* mp = &m.rgba[((size_t)py * m.w + px) * 4];
    CHECK(mp[3] == 255 && mp[0] > 200, "track pixel not red: a=%d r=%d", mp[3], mp[0]);
    const std::string wf = worldFile(m);
    double sx, z1, z2, sy, ox, oy;
    CHECK(sscanf(wf.c_str(), "%lf %lf %lf %lf %lf %lf", &sx, &z1, &z2, &sy, &ox, &oy) == 6 && sy < 0, "world file");
    CHECK(std::abs((mid.lon - ox) / sx - px) < 1.0 && std::abs((mid.lat - oy) / sy - py) < 1.0, "world file mapping");
    drawGraticule(m, 5, 0xFF00FFFFu);
    int g5 = 0;
    for (size_t i = 0; i < m.rgba.size(); i += 4) if (m.rgba[i] == 255 && m.rgba[i + 1] == 255 && m.rgba[i + 2] == 0) g5++;
    CHECK(g5 > 50, "graticule drawn %d", g5);

    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("imgproc OK\n");
    return 0;
}
