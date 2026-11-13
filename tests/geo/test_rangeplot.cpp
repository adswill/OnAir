// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Receiver range plot (rangeplot.h): bearing and distance, azimuth buckets with a maximum, rejection of absurd positions, the rolling window,
// the CSV and the RGBA picture. Pure maths, no radio.
#include "dect2/geo/rangeplot.h"
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

using dect2::RangePlot;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static bool pixelIs(const std::vector<uint8_t>& px, int size, int x, int y, int r, int g, int b) {
    const uint8_t* p = &px[((size_t)y * size + x) * 4];
    return p[0] == r && p[1] == g && p[2] == b && p[3] == 255;
}

int main() {
    // ---- bearing and distance
    {
        double az, km;
        RangePlot::bearingDistance(51.5074, -0.1278, 48.8566, 2.3522, az, km);   // London -> Paris: about 343.5 km, bearing about 148.1
        CHECK(std::fabs(km - 343.5) < 1.5 && std::fabs(az - 148.1) < 0.6, "London-Paris %.2f km %.2f deg", km, az);
        RangePlot::bearingDistance(0, 0, 0, 1, az, km);   // one degree of longitude on the equator: 111.19 km, due east
        CHECK(std::fabs(km - 111.195) < 0.05 && std::fabs(az - 90) < 1e-6, "equator %.3f km %.4f deg", km, az);
        RangePlot::bearingDistance(10, 20, 11, 20, az, km);
        CHECK(std::fabs(az) < 1e-6 && std::fabs(km - 111.195) < 0.05, "north %.3f km %.4f deg", km, az);
        RangePlot::bearingDistance(10, 20, 9, 20, az, km);
        CHECK(std::fabs(az - 180) < 1e-6, "south %.4f", az);
        RangePlot::bearingDistance(10, 20, 10, 19, az, km);
        CHECK(az > 269.9 && az < 270.1, "west %.4f", az);   // not exactly 270 on a sphere, but within a tenth of a degree for 1 degree
    }
    // ---- buckets: 5 degrees, wrap, maximum kept
    {
        RangePlot p;
        CHECK(p.buckets() == 72 && p.bucketDeg() == 5, "default buckets %d", p.buckets());
        CHECK(p.bucketOf(0) == 0 && p.bucketOf(4.99) == 0 && p.bucketOf(5) == 1 && p.bucketOf(359.99) == 71 && p.bucketOf(360) == 0 && p.bucketOf(-1) == 71 && p.bucketOf(725) == 1, "bucketOf");
        CHECK(p.add(2, 50), "first report stores");
        CHECK(!p.add(3, 40), "a shorter one in the same bucket changes nothing");
        CHECK(p.add(4, 80), "a longer one replaces");
        CHECK(p.maxKm(0) == 80 && p.bucketSamples(0) == 3, "bucket 0: %.1f km, %u reports", p.maxKm(0), p.bucketSamples(0));
        CHECK(p.maxKm(1) == 0, "bucket 1 empty");
        p.add(359, 120);
        CHECK(p.maxKm(71) == 120 && p.overallMaxKm() == 120 && std::fabs(p.farthestAzDeg() - 357.5) < 1e-9, "farthest %.1f at %.1f", p.overallMaxKm(), p.farthestAzDeg());
        CHECK(p.samples() == 4, "samples %llu", (unsigned long long)p.samples());
        p.reset();
        CHECK(p.overallMaxKm() == 0 && p.samples() == 0 && p.farthestAzDeg() < 0, "reset");
    }
    // ---- rejected input
    {
        RangePlot p(5, 0, 1000);
        CHECK(!p.add(10, -1), "negative distance");
        CHECK(!p.add(10, 1001), "over the sanity limit");
        CHECK(!p.add(NAN, 10) && !p.add(10, NAN) && !p.add(10, INFINITY), "not finite");
        CHECK(!p.addPosition(50, 8, 95, 8) && !p.addPosition(50, 8, 50, 200) && !p.addPosition(50, 8, NAN, 8), "impossible position");
        CHECK(p.samples() == 0, "nothing counted: %llu", (unsigned long long)p.samples());
        CHECK(p.addPosition(50, 8, 50, 9), "a good position");   // 71.5 km east (initial bearing 89.7: a great circle bows toward the pole)
        CHECK(p.bucketSamples(p.bucketOf(89.7)) == 1 && std::fabs(p.maxKm(p.bucketOf(89.7)) - 71.5) < 0.5, "east %.1f km", p.maxKm(p.bucketOf(89.7)));
    }
    // ---- other bucket widths
    {
        RangePlot a(10); CHECK(a.buckets() == 36, "10 deg: %d", a.buckets());
        RangePlot b(7); CHECK(b.buckets() == 72 && b.bucketDeg() == 5, "7 deg is not a divisor: back to 5");
        RangePlot c(0); CHECK(c.buckets() == 72, "0 deg");
        RangePlot d(1); CHECK(d.buckets() == 360, "1 deg");
        RangePlot e(90); CHECK(e.buckets() == 4, "90 deg");
    }
    // ---- rolling window: an old maximum runs out; a newer shorter report replaces it
    {
        RangePlot p(5, 3600);
        p.add(10, 200, 1000);
        CHECK(p.maxKm(2, 1000 + 3599) == 200, "inside the window");
        CHECK(p.maxKm(2, 1000 + 3601) == 0 && p.overallMaxKm(1000 + 3601) == 0, "outside the window");
        CHECK(p.add(11, 90, 1000 + 4000), "a shorter one replaces the expired maximum");
        CHECK(p.maxKm(2, 1000 + 4000) == 90, "now 90 km");
        p.add(11, 90, 1000 + 7000);   // heard at the same range again: stays fresh
        CHECK(p.maxKm(2, 1000 + 7000 + 3000) == 90 && p.maxKm(2, 1000 + 7000 + 3700) == 0, "refreshed by the same range");
    }
    // ---- CSV
    {
        RangePlot p;
        p.add(92, 185.2);
        const std::string s = p.toCsv();
        std::istringstream in(s);
        std::string line; int n = 0; std::string row18;
        while (std::getline(in, line)) { if (n == 0) CHECK(line == "azimuth_from_deg,azimuth_to_deg,azimuth_centre_deg,max_range_km,max_range_nm,reports", "header '%s'", line.c_str()); if (n == 19) row18 = line; n++; }
        CHECK(n == 73, "73 lines, got %d", n);
        CHECK(row18 == "90.0,95.0,92.5,185.20,100.00,1", "row '%s'", row18.c_str());
        CHECK(s.find("\n0.0,5.0,2.5,,,0\n") != std::string::npos, "an empty bucket has empty cells");
    }
    // ---- ring step and the picture
    {
        CHECK(RangePlot::ringStep(100) == 20 && RangePlot::ringStep(250) == 50 && RangePlot::ringStep(5) == 1 && RangePlot::ringStep(430) == 100 && RangePlot::ringStep(0) == 10, "ring steps %g %g %g %g",
              RangePlot::ringStep(100), RangePlot::ringStep(250), RangePlot::ringStep(5), RangePlot::ringStep(430));
        RangePlot p;
        for (double az = 80; az < 100; az += 1) p.add(az, 100);   // a spur to the east, 100 km
        p.add(272, 50);                                          // a short one to the west
        const int S = 480;
        const std::vector<uint8_t> px = p.renderRgba(S);
        CHECK(px.size() == (size_t)S * S * 4, "size");
        const double cx = (S - 1) / 2.0, R = S * 0.5 - 0.06 * S;   // outer ring = 100 km (5 rings of 20)
        // inside the eastern wedge at half way: filled; beyond its end: not filled; the western wedge only reaches half way
        CHECK(pixelIs(px, S, (int)(cx + R * 0.5), (int)cx + 3, 28, 88, 120), "east wedge filled");
        CHECK(!pixelIs(px, S, (int)(cx + R * 0.6), (int)(cx - R * 0.6), 28, 88, 120), "north-east is empty");
        CHECK(pixelIs(px, S, (int)(cx - R * 0.3), (int)cx - 3, 28, 88, 120), "west wedge filled near the middle");
        CHECK(!pixelIs(px, S, (int)(cx - R * 0.7), (int)cx - 3, 28, 88, 120), "west wedge ends at half range");
        // an empty plot still renders: background and rings only
        RangePlot q;
        const std::vector<uint8_t> e = q.renderRgba(200);
        CHECK(e.size() == 200u * 200 * 4 && e[3] == 255, "empty plot renders");
    }
    if (fails) { std::printf("%d failures\n", fails); return 1; }
    std::printf("rangeplot ok\n");
    return 0;
}
