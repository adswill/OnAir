// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RDS2 subcarriers and SCA audio in a simulated FM multiplex: groups come out with good checkwords at every stream, with noise and clock offset, and nothing is
// reported on a plain stereo + RDS station.
#include "dect2/fm/fm_subcarrier.h"
#include "dect2/common/stable_random.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static uint32_t crc(uint32_t d) { uint32_t r = d << 10; for (int i = 25; i >= 10; i--) if (r >> i & 1) r ^= 0x5B9u << (i - 10); return r & 0x3FF; }
static std::vector<int> bits(uint16_t seed, int groups) {
    static const uint32_t off[4] = {0x0FC, 0x198, 0x168, 0x1B4};
    std::vector<int> v; uint16_t c = seed;
    for (int g = 0; g < groups; g++) for (int b = 0; b < 4; b++) { c = (uint16_t)(c * 2053u + 13849u); uint32_t w = (uint32_t)c << 10 | (crc(c) ^ off[b]); for (int i = 25; i >= 0; i--) v.push_back(w >> i & 1); }
    return v;
}
struct Tx { double fHz; std::vector<int> b; int di = 0; double dev; };
static double bpskAt(const std::vector<int>& d, double t) {   // biphase symbol of differentially encoded bits, 1187.5 bit/s
    const double T = 1.0 / 1187.5; const double u = t / T; size_t k = (size_t)u; if (k >= d.size()) return 0;
    const double fr = u - (double)k; return (d[k] ? 1 : -1) * std::sin(2 * M_PI * fr);
}
static std::vector<int> diff(const std::vector<int>& b) { std::vector<int> d(b.size()); int p = 0; for (size_t i = 0; i < b.size(); i++) { p ^= b[i]; d[i] = p; } return d; }
struct Sig { std::vector<Tx> rds2; double sca1 = 0, sca2 = 0, noise = 0; double clockPpm = 0; };
static std::vector<float> mpx(const Sig& s, double fm, double secs) {
    std::vector<float> v((size_t)(secs * fm));
    std::mt19937 rng(11); dect2::normal_distribution<float> nd(0.f, 1.f);
    double p1 = 0, p2 = 0;
    std::vector<std::vector<int>> ds; for (auto& t : s.rds2) ds.push_back(diff(t.b));
    for (size_t i = 0; i < v.size(); i++) {
        const double t = i / fm * (1 + s.clockPpm * 1e-6);
        double m = 0.45 * std::sin(2 * M_PI * 1000 * t) + 0.09 * std::cos(2 * M_PI * 19000 * t) + 0.2 * std::sin(2 * M_PI * 1500 * t) * std::cos(2 * M_PI * 38000 * t);
        for (size_t k = 0; k < s.rds2.size(); k++) m += s.rds2[k].dev * bpskAt(ds[k], t) * std::cos(2 * M_PI * s.rds2[k].fHz * t);
        if (s.sca1 > 0) { const double f = 67000 + 6000 * std::sin(2 * M_PI * 700 * t); p1 = std::fmod(p1 + 2 * M_PI * f / fm, 2 * M_PI); m += s.sca1 * std::cos(p1); }
        if (s.sca2 > 0) { const double f = 92000 + 6000 * std::sin(2 * M_PI * 1100 * t); p2 = std::fmod(p2 + 2 * M_PI * f / fm, 2 * M_PI); m += s.sca2 * std::cos(p2); }
        v[i] = (float)m + (s.noise > 0 ? (float)s.noise * nd(rng) : 0.f);
    }
    return v;
}
int main() {
    const double fm = 250000;
    for (double noise : {0.0, 0.005, 0.012}) for (double ppm : {0.0, 80.0}) {
        Sig s; s.noise = noise; s.clockPpm = ppm;
        const double fr[3] = {66500, 71250, 76000};
        for (int i = 0; i < 3; i++) s.rds2.push_back({fr[i], bits((uint16_t)(0x1234 + i), 120), 0, 2.0 / 75});
        auto v = mpx(s, fm, 8);
        FmSubcarriers f; f.configure(fm); for (size_t o = 0; o < v.size(); o += 4096) f.feed(v.data() + o, std::min<size_t>(4096, v.size() - o));
        auto st = f.status();
        for (int i = 0; i < 3; i++) {
            printf("noise %.3f %+.0f ppm stream %d: sync %d ok %.0f%% groups %llu\n", noise, ppm, i + 1, st.rds2[i].synced, st.rds2[i].okPct, (unsigned long long)st.rds2[i].groups);
            CHECK(st.rds2[i].synced && st.rds2[i].okPct > 80 && st.rds2[i].groups > 40, "stream %d at noise %.3f, %.0f ppm", i + 1, noise, ppm);
        }
    }
    {   // SCA
        for (double noise : {0.0, 0.01}) {
            Sig s; s.sca1 = 0.1; s.sca2 = 0.05; s.noise = noise;
            auto v = mpx(s, fm, 4);
            FmSubcarriers f; f.configure(fm); f.setScaListen(0);
            for (size_t o = 0; o < v.size(); o += 4096) f.feed(v.data() + o, std::min<size_t>(4096, v.size() - o));
            auto st = f.status();
            std::vector<float> a; f.takeScaAudio(a);
            std::complex<double> z = 0; size_t n = 0;
            for (size_t i = a.size() / 2; i < a.size(); i++, n++) z += (double)a[i] * std::polar(1.0, -2 * M_PI * 700.0 * (double)i / 48000);
            printf("SCA noise %.3f: 67 %d (%.0f dB) 92 %d (%.0f dB), audio %zu samples, tone %.3f\n", noise, st.sca[0].present, st.sca[0].snrDb, st.sca[1].present, st.sca[1].snrDb, a.size(), 2 * std::abs(z) / n);
            CHECK(st.sca[0].present && st.sca[1].present, "SCA not found at noise %.3f", noise);
            CHECK(a.size() > 180000 && a.size() < 200000, "audio %zu", a.size());
            CHECK(2 * std::abs(z) / n > 0.55, "SCA tone %.3f", 2 * std::abs(z) / n);
        }
    }
    {   // nothing there
        Sig s; s.noise = 0.003;
        auto v = mpx(s, fm, 6);
        FmSubcarriers f; f.configure(fm); for (size_t o = 0; o < v.size(); o += 4096) f.feed(v.data() + o, std::min<size_t>(4096, v.size() - o));
        auto st = f.status();
        for (int i = 0; i < 3; i++) CHECK(!st.rds2[i].synced, "false RDS2 on stream %d", i + 1);
        for (int i = 0; i < 2; i++) CHECK(!st.sca[i].present, "false SCA %d (%.0f dB)", i, st.sca[i].snrDb);
    }
    printf(fails ? "fm subcarrier FAILED\n" : "fm subcarrier passed\n");
    return fails ? 1 : 0;
}
