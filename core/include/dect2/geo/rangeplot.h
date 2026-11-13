// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Receiver range plot: the farthest position heard in each azimuth bucket (5 degrees by default) from the station, for ADS-B and AIS.
// Pure and header-only. add() keeps the maximum per bucket; with a window (windowSec > 0) a bucket's maximum is "rolling": it counts only
// while it is younger than the window, and a newer, shorter report replaces an expired maximum. Also: the bearing and great-circle
// distance between two positions, CSV text, and a plain RGBA rendering (rings, N/E/S/W spokes, filled wedges, ring labels) for PNG export.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

class RangePlot {
public:
    explicit RangePlot(double bucketDeg = 5.0, double windowSec = 0.0, double maxSaneKm = 1500.0) { configure(bucketDeg, windowSec, maxSaneKm); }

    void configure(double bucketDeg, double windowSec = 0.0, double maxSaneKm = 1500.0) {
        // the bucket width must divide 360 into at least 4 and at most 360 whole buckets; anything else becomes 5 degrees
        int n = (int)std::lround(360.0 / bucketDeg);
        if (!(bucketDeg > 0) || n < 4 || n > 360 || std::fabs(n * bucketDeg - 360.0) > 1e-6) { bucketDeg = 5.0; n = 72; }
        bucketDeg_ = bucketDeg; windowSec_ = windowSec > 0 ? windowSec : 0; maxSaneKm_ = maxSaneKm;
        maxKm_.assign((size_t)n, 0.0); atSec_.assign((size_t)n, 0.0); count_.assign((size_t)n, 0u);
        total_ = 0;
    }
    void setWindow(double windowSec) { windowSec_ = windowSec > 0 ? windowSec : 0; }   // keeps what was heard; expiry applies from now on
    void reset() { configure(bucketDeg_, windowSec_, maxSaneKm_); }

    int buckets() const { return (int)maxKm_.size(); }
    double bucketDeg() const { return bucketDeg_; }
    double windowSec() const { return windowSec_; }
    uint64_t samples() const { return total_; }
    uint32_t bucketSamples(int b) const { return count_[(size_t)b]; }

    int bucketOf(double azDeg) const {
        double a = std::fmod(azDeg, 360.0);
        if (a < 0) a += 360.0;
        int b = (int)(a / bucketDeg_);
        return std::min(b, buckets() - 1);
    }

    // one report: bearing from the station (degrees from north, clockwise) and distance (km). False when it is rejected (not finite,
    // negative, over the sanity limit: a bad position decode must not stretch the plot) or changes nothing.
    bool add(double azDeg, double distKm, double tSec = 0) {
        if (!std::isfinite(azDeg) || !std::isfinite(distKm) || distKm < 0 || distKm > maxSaneKm_) return false;
        const size_t b = (size_t)bucketOf(azDeg);
        total_++; count_[b]++;
        const bool expired = windowSec_ > 0 && maxKm_[b] > 0 && tSec - atSec_[b] > windowSec_;
        if (distKm > maxKm_[b] || expired) { maxKm_[b] = distKm; atSec_[b] = tSec; return true; }
        if (distKm == maxKm_[b]) atSec_[b] = std::max(atSec_[b], tSec);   // heard again at the same range: still fresh
        return false;
    }
    bool addPosition(double obsLat, double obsLon, double lat, double lon, double tSec = 0) {
        if (!std::isfinite(lat) || !std::isfinite(lon) || std::fabs(lat) > 90 || std::fabs(lon) > 180) return false;
        double az, km;
        bearingDistance(obsLat, obsLon, lat, lon, az, km);
        return add(az, km, tSec);
    }

    // the maximum of a bucket now (0: nothing, or the window has run out)
    double maxKm(int b, double nowSec = 0) const {
        const size_t i = (size_t)b;
        if (maxKm_[i] <= 0) return 0;
        if (windowSec_ > 0 && nowSec - atSec_[i] > windowSec_) return 0;
        return maxKm_[i];
    }
    double overallMaxKm(double nowSec = 0) const {
        double m = 0;
        for (int b = 0; b < buckets(); b++) m = std::max(m, maxKm(b, nowSec));
        return m;
    }
    // the bearing (degrees) of the farthest bucket's centre, -1 when empty
    double farthestAzDeg(double nowSec = 0) const {
        int best = -1; double m = 0;
        for (int b = 0; b < buckets(); b++) if (maxKm(b, nowSec) > m) { m = maxKm(b, nowSec); best = b; }
        return best < 0 ? -1.0 : (best + 0.5) * bucketDeg_;
    }

    // initial bearing (degrees from north) and great-circle distance (km, mean earth radius) from (lat0, lon0) to (lat1, lon1)
    static void bearingDistance(double lat0, double lon0, double lat1, double lon1, double& azDeg, double& km) {
        const double d2r = dect2::math::kPi / 180.0;
        const double p0 = lat0 * d2r, p1 = lat1 * d2r, dl = (lon1 - lon0) * d2r;
        const double sa = std::sin((p1 - p0) / 2), sb = std::sin(dl / 2);
        double h = sa * sa + std::cos(p0) * std::cos(p1) * sb * sb;
        h = std::min(1.0, std::max(0.0, h));
        km = 2 * 6371.0088 * std::asin(std::sqrt(h));
        const double y = std::sin(dl) * std::cos(p1), x = std::cos(p0) * std::sin(p1) - std::sin(p0) * std::cos(p1) * std::cos(dl);
        azDeg = std::atan2(y, x) / d2r;
        if (azDeg < 0) azDeg += 360.0;
    }

    // CSV (LF line ends): one row per bucket, ranges in km and nautical miles (empty cell when nothing was heard there)
    std::string toCsv(double nowSec = 0) const {
        std::string s = "azimuth_from_deg,azimuth_to_deg,azimuth_centre_deg,max_range_km,max_range_nm,reports\n";
        for (int b = 0; b < buckets(); b++) {
            char l[160];
            const double m = maxKm(b, nowSec);
            if (m > 0) std::snprintf(l, sizeof l, "%.1f,%.1f,%.1f,%.2f,%.2f,%u\n", b * bucketDeg_, (b + 1) * bucketDeg_, (b + 0.5) * bucketDeg_, m, m / 1.852, (unsigned)count_[(size_t)b]);
            else std::snprintf(l, sizeof l, "%.1f,%.1f,%.1f,,,%u\n", b * bucketDeg_, (b + 1) * bucketDeg_, (b + 0.5) * bucketDeg_, (unsigned)count_[(size_t)b]);
            s += l;
        }
        return s;
    }

    // The ring step for a plot reaching maxKm: 1, 2, 5 times a power of ten (in the plot's unit) with at most 5 rings.
    static double ringStep(double maxUnits) {
        if (!(maxUnits > 0)) return 10;
        for (double p = 1; p < 1e7; p *= 10)
            for (double m : {1.0, 2.0, 5.0}) if (maxUnits / (m * p) <= 5.0 + 1e-9) return m * p;
        return 1e7;
    }

    // size x size RGBA picture: north up, rings every ringStep(), the outer ring just past the farthest bucket. unitPerKm turns km into the
    // label unit (1: km, 0.5399568: nautical miles). The result is for writePng().
    std::vector<uint8_t> renderRgba(int size, double nowSec = 0, double unitPerKm = 1.0) const {
        size = std::max(64, size);
        std::vector<uint8_t> px((size_t)size * size * 4);
        const uint8_t bg[3] = {12, 14, 16}, ring[3] = {70, 76, 80}, fill[3] = {28, 88, 120}, edge[3] = {70, 190, 230}, txt[3] = {200, 205, 210};
        auto put = [&](int x, int y, const uint8_t* c) { if (x < 0 || y < 0 || x >= size || y >= size) return; uint8_t* p = &px[((size_t)y * size + x) * 4]; p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; p[3] = 255; };
        for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) put(x, y, bg);
        const double maxU = overallMaxKm(nowSec) * unitPerKm, step = ringStep(maxU > 0 ? maxU : 10);
        const int nRings = std::max(1, (int)std::ceil(maxU / step - 1e-9));
        const double scaleU = nRings * step;   // the outer ring
        const double cx = (size - 1) / 2.0, cy = cx, R = size * 0.5 - 0.06 * size;   // margin for the labels
        const double pxPerKm = R / (scaleU / unitPerKm);
        // wedges
        for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) {
            const double dx = x - cx, dy = cy - y, r = std::sqrt(dx * dx + dy * dy);
            if (r < 1 || r > R + 2) continue;
            const double az = std::atan2(dx, dy) * 180.0 / dect2::math::kPi;
            const int b = bucketOf(az);
            const double rb = maxKm(b, nowSec) * pxPerKm;
            if (rb <= 0) continue;
            if (r <= rb - 1.5) put(x, y, fill);
            else if (r <= rb + 0.5) put(x, y, edge);
        }
        // rings and spokes
        const int dots = 720;
        for (int k = 1; k <= nRings; k++) for (int i = 0; i < dots; i++) {
            const double a = i * 2 * dect2::math::kPi / dots, r = R * k / nRings;
            put((int)std::lround(cx + r * std::sin(a)), (int)std::lround(cy - r * std::cos(a)), ring);
        }
        for (int i = -(int)R; i <= (int)R; i++) { put((int)std::lround(cx) + i, (int)std::lround(cy), ring); put((int)std::lround(cx), (int)std::lround(cy) + i, ring); }
        // labels: ring values and N E S W, in a 3 x 5 pixel font scaled up
        const int sc = std::max(1, size / 240);
        for (int k = 1; k <= nRings; k++) {
            char b[24]; std::snprintf(b, sizeof b, "%g", step * k);
            drawText(px, size, (int)cx + 3 * sc, (int)(cy - R * k / nRings) - 6 * sc, b, sc, txt);
        }
        drawText(px, size, (int)cx - 2 * sc, (int)(cy - R) - 8 * sc - 2, "N", sc, txt);
        drawText(px, size, (int)(cx + R) + 3 * sc, (int)cy - 3 * sc, "E", sc, txt);
        drawText(px, size, (int)cx - 2 * sc, (int)(cy + R) + 3 * sc, "S", sc, txt);
        drawText(px, size, (int)(cx - R) - 6 * sc, (int)cy - 3 * sc, "W", sc, txt);
        return px;
    }

private:
    double bucketDeg_ = 5, windowSec_ = 0, maxSaneKm_ = 1500;
    std::vector<double> maxKm_, atSec_;
    std::vector<uint32_t> count_;
    uint64_t total_ = 0;

    // 3 x 5 glyphs, rows top to bottom, 3 bits each (bit 2 = left)
    static const uint8_t* glyph(char c) {
        static const uint8_t g[][5] = {
            {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},  // 0-9
            {0, 0, 0, 0, 2},   // .
        };
        static const uint8_t N[5] = {5, 7, 7, 7, 5}, E[5] = {7, 4, 6, 4, 7}, S[5] = {7, 4, 7, 1, 7}, W[5] = {5, 5, 7, 7, 5}, none[5] = {0, 0, 0, 0, 0};
        if (c >= '0' && c <= '9') return g[c - '0'];
        if (c == '.') return g[10];
        if (c == 'N') return N;
        if (c == 'E') return E;
        if (c == 'S') return S;
        if (c == 'W') return W;
        return none;
    }
    static void drawText(std::vector<uint8_t>& px, int size, int x0, int y0, const char* s, int sc, const uint8_t* col) {
        for (; *s; s++, x0 += 4 * sc) {
            const uint8_t* g = glyph(*s);
            for (int r = 0; r < 5; r++) for (int c = 0; c < 3; c++) if (g[r] & (4 >> c))
                for (int dy = 0; dy < sc; dy++) for (int dx = 0; dx < sc; dx++) {
                    const int x = x0 + c * sc + dx, y = y0 + r * sc + dy;
                    if (x < 0 || y < 0 || x >= size || y >= size) continue;
                    uint8_t* p = &px[((size_t)y * size + x) * 4]; p[0] = col[0]; p[1] = col[1]; p[2] = col[2]; p[3] = 255;
                }
        }
    }
};

}  // namespace dect2
