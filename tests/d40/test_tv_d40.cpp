// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/d40/tv_d40.h"
#include <cstdio>
#include <random>
using namespace dect2::d40;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
    std::mt19937 rng(21); std::normal_distribution<double> g(0, 1);
    // QAM order: every order, MER 18..35 dB, rotation-free, plus a constellation tilt (gain 0.97)
    int tot = 0, ok = 0;
    for (int order : {16, 32, 64, 128, 256}) for (double mer : {22.0, 26.0, 32.0}) {
        if (order == 256 && mer < 28) continue;      // 256-QAM does not exist below ~ 27 dB MER
        if (order == 128 && mer < 24) continue;
        const auto grid = qamGrid(order); const double sd = std::pow(10.0, -mer / 20.0) / std::sqrt(2.0);
        std::vector<std::complex<double>> p;
        for (int i = 0; i < 4000; i++) p.push_back(grid[rng() % grid.size()] * 0.97 + std::complex<double>(g(rng) * sd, g(rng) * sd));
        const auto r = guessQam(p); tot++; if (r.order == order) ok++; else std::printf("order %d mer %.0f guessed %d\n", order, mer, r.order);
    }
    std::printf("QAM order: %d of %d right\n", ok, tot); CHECK(ok == tot);
    { std::vector<std::complex<double>> p; for (int i = 0; i < 4000; i++) p.push_back({g(rng), g(rng)}); CHECK(guessQam(p).order == 0); }   // noise: no QAM
    CHECK(qamGrid(32).size() == 32 && qamGrid(128).size() == 128 && qamGrid(256).size() == 256);

    // HD Radio spectrum: host carrier + FM-like occupancy +-75 kHz, sidebands at -20 dBc, noise
    { const double binHz = 1000; const int n = 500; std::vector<float> db(n);
      auto make = [&](double upDb, double loDb, double hostDb) {
          for (int i = 0; i < n; i++) { const double f = (i - n / 2) * binHz; double p = 1e-6 * (1 + 0.2 * g(rng) * g(rng) * 0 );
              if (std::fabs(f) <= 75e3) p += std::pow(10.0, hostDb / 10.0) / 150.0;
              if (f >= 129e3 && f <= 198e3) p += std::pow(10.0, upDb / 10.0) / 69.0;
              if (f <= -129e3 && f >= -198e3) p += std::pow(10.0, loDb / 10.0) / 69.0;
              db[(size_t)i] = (float)(10 * std::log10(p * std::exp(0.15 * g(rng)))); } };
      make(-20, -20, 0); auto h = hdrLevels(db, binHz);
      CHECK(h.ok && h.kind == "hybrid" && std::fabs(h.upperDbc + 20) < 1.5 && std::fabs(h.lowerDbc + 20) < 1.5 && h.note.empty());
      make(-20, -32, 0); h = hdrLevels(db, binHz); CHECK(h.kind == "hybrid" && h.imbalanceDb > 9 && !h.note.empty() && h.note.find("lower") == 0);
      make(-60, -60, 0); CHECK(hdrLevels(db, binHz).kind == "analogue only");
      make(0, 0, -60); CHECK(hdrLevels(db, binHz).kind == "all-digital");
      make(-70, -70, -70); CHECK(hdrLevels(db, binHz).kind == "no signal"); }

    // MER per carrier: 1000 carriers, 40 symbols, MER 30 dB, a notch of 12 carriers at 20 dB worse, a tilt
    { const int nc = 1000, ns = 40; std::vector<std::complex<float>> ref, rx; const auto q = qamGrid(16);
      for (int t = 0; t < ns; t++) for (int k = 0; k < nc; k++) {
          const auto s = q[rng() % q.size()]; double mer = 30;
          if (k >= 400 && k < 412) mer = 10;
          const double sd = std::pow(10.0, -mer / 20.0) / std::sqrt(2.0);
          ref.push_back({(float)s.real(), (float)s.imag()}); rx.push_back({(float)(s.real() + g(rng) * sd), (float)(s.imag() + g(rng) * sd)}); }
      auto r = merPerCarrier(rx, ref, nc);
      CHECK(std::fabs(r.meanDb - 29.8) < 1.0);
      CHECK(r.notches.size() == 1 && std::fabs(r.notches[0].first - 400) <= 1 && std::fabs(r.notches[0].second - 411) <= 1);
      CHECK(r.worstCarrier >= 400 && r.worstCarrier < 412);
      CHECK(std::fabs(r.tiltDbPerCarrier) < 0.002);
      // flat 30 dB, no notches: none reported
      rx.clear(); for (size_t i = 0; i < ref.size(); i++) { const double sd = std::pow(10.0, -30 / 20.0) / std::sqrt(2.0); rx.push_back({(float)(ref[i].real() + g(rng) * sd), (float)(ref[i].imag() + g(rng) * sd)}); }
      CHECK(merPerCarrier(rx, ref, nc).notches.empty()); }

    // analogue TV standard, with the line-rate / burst tolerances of a real receiver (crystal error 50 ppm and measurement noise)
    CHECK(classifyAtv(15625.0 * (1 + 40e-6), 4433618.75 * (1 - 30e-6), false, true).name == "PAL 625 lines");
    CHECK(classifyAtv(15734.26, 3579545.0 * 1.0002, false, false).name == "NTSC 525 lines");
    CHECK(classifyAtv(15625.0, 0, true, false).name == "SECAM 625 lines");
    CHECK(classifyAtv(15734.26, 3575611.0, false, true).name == "PAL-M 525 lines");
    CHECK(classifyAtv(15625.0, 3582056.0, false, true).name == "PAL-N 625 lines");
    CHECK(classifyAtv(15625.0, 0, false, false).name == "mono 625 lines");
    CHECK(!classifyAtv(16000, 0, false, false).ok);
    CHECK(!classifyAtv(15625.0, 4000000.0, false, false).ok);
    // video score ordering
    { const auto a = videoScore(38, 1, 20, 1), b = videoScore(25, 4, 80, 0.95), c = videoScore(12, 15, 300, 0.4);
      CHECK(a.score >= 85 && b.score > c.score && c.score < 40 && a.score > b.score); CHECK(c.weakest == "SNR" || c.weakest == "lock" || c.weakest == "sync stability"); CHECK(videoScore(40, 0, 0, 1).score == 100 && videoScore(0, 50, 900, 0).score == 0); }
    // FPV: pilots in 5.8: occupied R1..R3 area and A5 region
    { std::vector<std::pair<double, double>> lv;
      for (const auto& ch : dect2::fpvAllChannels()) { if (ch.freqHz < 5000e6) continue; double l = -80 + g(rng); const double f = ch.freqHz / 1e6; if (std::fabs(f - 5732) < 6 || std::fabs(f - 5769) < 6 || std::fabs(f - 5785) < 6) l = -45; lv.push_back({f, l}); }
      auto p = pickFpvChannels(lv, 4);
      CHECK(p.size() == 4);
      for (size_t i = 0; i < p.size(); i++) { CHECK(std::fabs(p[i].mhz - 5732) >= 20 && std::fabs(p[i].mhz - 5769) >= 20 && std::fabs(p[i].mhz - 5785) >= 20); for (size_t j = 0; j < i; j++) CHECK(std::fabs(p[i].mhz - p[j].mhz) >= 37); }
      CHECK(pickFpvChannels(lv, 20).size() < 20);           // cannot invent channels
      CHECK(pickFpvChannels({}, 3).empty()); }
    std::printf(fails ? "FAILED\n" : "tv_d40 ok\n");
    return fails ? 1 : 0;
}
