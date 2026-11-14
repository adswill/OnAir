// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Robust Packet (RPR) physical layer on the generator's signals, with the exact symbols known: the comb centre, the bursts, the speed level and the
// differential symbols, then the impairments (SNR, tuning error of +-50 Hz, clock error, fading) for both speed levels, and noise, a tone and a
// two-tone signal alone (nothing may be found). The generator's bursts are not real RPR frames (the coding is not public).
#include "dect2/rpr/rpr_dsp.h"
#include "dect2/rpr/rpr_gen.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/test_long.h"

static std::vector<float> audio(RprGenConfig c, double sec) {
    c.rate = rpr::kFs; c.analytic = false;
    RprGenerator g(c);
    std::vector<cf32> o((size_t)(sec * c.rate));
    g.generate(o.data(), o.size());
    std::vector<float> x(o.size());
    for (size_t i = 0; i < x.size(); i++) x[i] = o[i].real();
    return x;
}

// agreement of the recovered symbols of a burst with the sent ones: the best start shift within +-8 symbols over the 8 carriers
static double symAcc(const RprGenerator& g, const rpr::Burst& b, int level, double periodSec) {
    const int S = 1 + 8 + 60;                       // the generator's default: reference + preamble + payload
    const long idx = std::lround((b.startSec - 0.04) / periodSec);
    if (idx < 0 || idx >= 400) return 0;
    double best = 0;
    for (int sh = -8; sh <= 8; sh++) {
        size_t m = 0, t = 0;
        for (size_t k = 0; k < 8; k++) {
            const auto& sent = g.sent()[k];
            for (size_t i = 2; i + 2 < b.sym[k].size(); i++) {
                const long j = (long)i + sh;
                if (j < 0 || j >= S - 1) continue;
                t++;
                if (sent[(size_t)idx * (size_t)(S - 1) + (size_t)j] == b.sym[k][i]) m++;
            }
        }
        if (t > 200) best = std::max(best, (double)m / (double)t);
    }
    (void)level;
    return best;
}

struct R { bool found; double centre; int bursts, right, total; double acc; };

static R run(int level, double snr, double cfo, double ppm, double fade, double sec = 30) {
    RprGenConfig c; c.level = level; c.snrDb = snr; c.cfoHz = cfo; c.clockPpm = ppm; c.fadeHz = fade;
    const auto x = audio(c, sec);
    RprGenerator g(c);
    const rpr::Analysis a = rpr::analyse(x.data(), x.size());
    R r{a.found, a.centreHz, (int)a.bursts.size(), 0, 0, 0};
    double acc = 0; int na = 0;
    for (const auto& b : a.bursts) {
        r.total++;
        if (b.level == level) r.right++;
        acc += symAcc(g, b, level, g.periodSec()); na++;
    }
    r.acc = na ? acc / na : 0;
    return r;
}

int main() {
    const bool L = testLong();
    {   // clean: every burst, the right level, the exact symbols
        for (int lv = 1; lv <= 2; lv++) {
            const R r = run(lv, 99, 0, 0, 0);
            printf("  level %d clean: found %d centre %.1f bursts %d right %d symbols %.3f\n", lv, r.found, r.centre, r.bursts, r.right, r.acc);
            CHECK(r.found && std::fabs(r.centre - 1500) < 3, "level %d clean: centre %.1f", lv, r.centre);
            CHECK(r.bursts >= 9 && r.right == r.total, "level %d clean: %d bursts, %d with the right level", lv, r.bursts, r.right);
            CHECK(r.acc > 0.99, "level %d clean: symbols %.3f", lv, r.acc);
        }
    }
    {   // SNR (in 2.5 kHz) and tuning error, both levels
        
        const double cfos[] = {-50, 0, 50};
        for (int lv = 1; lv <= 2; lv++)
            for (double snr : {20.0, 8.0, lv == 1 ? 3.0 : 5.0}) for (double cfo : cfos) {
                if (!L && cfo == 0 && snr == 8) continue;   // quick run: the middle of the grid is covered by the long run
                const R r = run(lv, snr, cfo, 0, 0, L ? 30 : 20);
                const double need = snr >= 20 ? 0.97 : snr >= 8 ? 0.95 : 0.70;
                printf("  level %d snr %2.0f cfo %+3.0f: found %d centre %.1f bursts %d right %d symbols %.3f\n", lv, snr, cfo, r.found, r.centre, r.bursts, r.right, r.acc);
                CHECK(r.found && std::fabs(r.centre - (1500 + cfo)) < 4, "level %d snr %.0f cfo %.0f: centre %.1f", lv, snr, cfo, r.centre);
                CHECK(r.bursts >= (snr >= 8 ? 4 : 2) && r.right * 10 >= r.total * 9, "level %d snr %.0f cfo %.0f: %d bursts, %d right level", lv, snr, cfo, r.bursts, r.right);
                CHECK(r.acc >= need, "level %d snr %.0f cfo %.0f: symbols %.3f < %.2f", lv, snr, cfo, r.acc, need);
            }
    }
    {   // clock error of the sound card / radio and fading
        for (int lv = 1; lv <= 2; lv++) {
            const R a = run(lv, 20, 20, 150, 0, 20);
            CHECK(a.found && a.bursts >= 4 && a.right * 10 >= a.total * 9 && a.acc > 0.9, "level %d clock +150 ppm: bursts %d right %d symbols %.3f", lv, a.bursts, a.right, a.acc);
            const R f = run(lv, 20, 0, 0, 0.5, 20);
            printf("  level %d fading 0.5 Hz: bursts %d right %d symbols %.3f\n", lv, f.bursts, f.right, f.acc);
            CHECK(f.found && f.bursts >= 3 && f.right * 10 >= f.total * 8, "level %d fading: bursts %d right %d", lv, f.bursts, f.right);
        }
    }
    {   // nothing: noise, one tone, two tones, 8 unmodulated carriers (no symbol clock)
        std::mt19937 rng(5);
        std::normal_distribution<float> nd(0.f, 0.1f);
        std::vector<float> x(8000 * 30);
        for (auto& v : x) v = nd(rng);
        CHECK(!rpr::analyse(x.data(), x.size()).found, "noise");
        for (size_t i = 0; i < x.size(); i++) x[i] += 0.2f * std::sin(2 * 3.14159265 * 1500.0 * (double)i / 8000.0);
        CHECK(!rpr::analyse(x.data(), x.size()).found, "tone plus noise");
        for (size_t i = 0; i < x.size(); i++) x[i] = nd(rng) + 0.2f * std::sin(2 * 3.14159265 * 1290.0 * (double)i / 8000.0) + 0.2f * std::sin(2 * 3.14159265 * 1710.0 * (double)i / 8000.0);
        CHECK(!rpr::analyse(x.data(), x.size()).found, "two tones plus noise");
        const auto cw = rpr::carriersFor(1500);
        for (size_t i = 0; i < x.size(); i++) { double v = nd(rng); for (double f : cw) v += 0.05 * std::sin(2 * 3.14159265 * f * (double)i / 8000.0); x[i] = (float)v; }
        CHECK(!rpr::analyse(x.data(), x.size()).found, "eight unmodulated carriers");
        std::vector<float> s(100);
        CHECK(!rpr::analyse(s.data(), s.size()).found, "too short");
    }
    {   // the carrier frequencies: eight, 60 Hz apart, centred
        const auto c = rpr::carriersFor(1500);
        CHECK(c.size() == 8 && std::fabs(c[0] - 1290) < 1e-9 && std::fabs(c[7] - 1710) < 1e-9 && std::fabs(c[1] - c[0] - 60) < 1e-9, "carrier set");
    }
    return testSummary("rpr dsp");
}
