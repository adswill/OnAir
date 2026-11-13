// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/d40/sat_e40.h"
#include <cstdio>
#include <random>
using namespace dect2::d40;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
    std::mt19937 rng(5); std::normal_distribution<float> nd(0, 1);
    // stitch: 20 trials, noisy, with a gain/offset change between passes; and unrelated strips must not match
    int right = 0, falseOk = 0;
    for (int t = 0; t < 20; t++) {
        std::vector<float> img(400); float v = 0; for (auto& x : img) { v += nd(rng) * 4; v *= 0.97f; x = v; }
        const int lag = 150 + (int)(rng() % 100);
        std::vector<float> a(img.begin(), img.begin() + 380), b(img.begin() + lag, img.begin() + lag + 120);
        for (auto& x : a) x += nd(rng) * 1.0f; for (auto& x : b) x = 0.8f * x + 5 + nd(rng) * 1.0f;
        auto r = stitchLag(a, b, 30); if (r.ok && r.lag == lag) right++;
        std::vector<float> u(120); float w = 0; for (auto& x : u) { w += nd(rng) * 4; w *= 0.97f; x = w; }
        if (stitchLag(a, u, 30, 0.9).ok) falseOk++;
    }
    CHECK(right >= 19); CHECK(falseOk == 0);
    // link budget: FSPL at 137.1 MHz, 850 km overhead = 32.44 + 20log10(850) + 20log10(137.1) = 133.8 dB
    auto lb = passLinkBudget(137.1e6, 6.0, 0.0, 300, 40e3, 850, 90);
    CHECK(std::fabs(lb.rangeKm - 850) < 0.01); CHECK(std::fabs(lb.fsplDb - 133.77) < 0.1);
    auto lo = passLinkBudget(137.1e6, 6.0, 0.0, 300, 40e3, 850, 5);
    CHECK(lo.rangeKm > 2850 && lo.rangeKm < 2930); CHECK(lo.snrDb < lb.snrDb - 8);
    const double need = lo.snrDb + 1;      // the SNR at 5 deg plus 1 dB: threshold elevation must sit just above 5 deg
    const double me = minElevationFor(need, 137.1e6, 6.0, 0.0, 300, 40e3, 850); CHECK(me > 5 && me < 10);
    CHECK(minElevationFor(100, 137.1e6, 6, 0, 300, 40e3, 850) < 0); CHECK(minElevationFor(-100, 137.1e6, 6, 0, 300, 40e3, 850) == 0);
    // names
    DecoderHint h; CHECK(decoderHint("NOAA 19", h) && h.decoder == "APT" && h.freqHz == 137.1e6);
    CHECK(decoderHint("noaa-18 (APT)", h) && h.freqHz == 137.9125e6); CHECK(decoderHint("METEOR-M2 3", h) && h.decoder == "LRPT");
    CHECK(decoderHint("ISS (ZARYA)", h) && h.baud == 1200); CHECK(!decoderHint("NO-SUCH-SAT", h)); CHECK(!decoderHint("", h));
    // log
    ObsLog L; L.add({"NOAA 19", 1000, 1900, 70, 20, 22, 30}); L.add({"NOAA 19", 5000, 5800, 20, -1, 0, 9}); L.add({"NOAA 19", 9000, 9500, 10, -1, 0, 1}); L.add({"NOAA 19", 12000, 12900, 45, 15, 5, 12});
    CHECK(L.verdict({"x", 0, 0, 0, -1, 0, 9.0}) == ObsVerdict::Bad); CHECK(std::fabs(L.successRate("NOAA 19") - 0.5) < 1e-9);
    CHECK(L.lowestDecodedElevation("NOAA 19") == 15); CHECK(L.lowestDecodedElevation("none") == -1);
    CHECK(L.csv().find("bad") != std::string::npos && L.csv().find("failed") != std::string::npos);
    // sun and shadow: at the March equinox the Sun is near +x; satellite at 7000 km
    double sun[3]; sunUnit(2451545.0 + 7305.0 + 79.0, sun);   // 2020-03-20 about 03:00 UTC (JD 2458928.6 or so)
    CHECK(std::fabs(sun[0]) > 0.99 && std::fabs(sun[2]) < 0.05);
    double s1[3] = {sun[0] * 7000, sun[1] * 7000, sun[2] * 7000}, s2[3] = {-sun[0] * 7000, -sun[1] * 7000, -sun[2] * 7000};
    double s3[3] = {sun[1] * 7000, -sun[0] * 7000, 0};       // 90 deg off the Sun line, 7000 km: outside the 6378 km cylinder
    CHECK(sunlit(s1, sun)); CHECK(!sunlit(s2, sun)); CHECK(sunlit(s3, sun));
    // retune: NOAA-like Doppler curve +-3.4 kHz over 12 minutes
    std::vector<std::pair<double, double>> d; for (int i = 0; i <= 720; i++) d.push_back({(double)i, -3400 * std::tanh((i - 360) / 120.0)});
    auto wide = planRetunes(d, 40e3), narrow = planRetunes(d, 2e3);
    CHECK(wide.size() < narrow.size()); CHECK(wide.size() >= 2 && wide.size() <= 12);
    for (auto& s : d) { double held = 0; for (auto& r : narrow) if (r.t <= s.first) held = r.offsetHz; CHECK(std::fabs(s.second - held) <= 0.25 * 2e3 / 2 + 1e-9); }
    if (!fails) std::printf("sat_e40 ok\n"); return fails ? 1 : 0;
}
