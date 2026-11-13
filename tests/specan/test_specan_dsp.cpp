// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Spectrum analyser maths: the grid, stitching across step edges, spur removal, peak hold and average, CSV, allocations, ticks,
// and the stepped receiver over a synthetic band (the retune callback moves a made-up radio).
#include "dect2/specan/specan_dsp.h"
#include "dect2/common/stable_random.h"
#include "dect2/specan/specan_rx.h"
#include "dect2/specan/specan_tel.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    {   // grid
        SaGrid g = saMakeGrid(100e6, 110e6, 1e5);
        CHECK(g.bins == 100 && g.index(100.05e6) == 0 && g.index(109.99e6) == 99 && std::fabs(g.centre(0) - 100.05e6) < 1, "grid %zu", g.bins);
        SaGrid w = saMakeGrid(1e6, 6000e6, 1e3, 200000);
        CHECK(w.bins <= 200001 && w.binHz > 1e3, "bin widened %.0f", w.binHz);
    }
    {   // CSV carries every bin at the chosen resolution (no 30 kHz cap on 1 MHz .. 6 GHz at 5 kHz)
        SaGrid g = saMakeGrid(1e6, 6000e6, 5e3);
        CHECK(g.binHz == 5e3 && g.bins == 1199800, "5k grid %.0f %zu", g.binHz, g.bins);
        SaGrid f = saMakeGrid(1e6, 6000e6, 2.8e3);
        CHECK(f.binHz == 2.8e3 && f.bins > 2000000, "2.8k grid %.0f %zu", f.binHz, f.bins);
        std::vector<float> v(g.bins, -50.f);
        size_t rows = 0, bytes = 0;
        double first = 0, second = 0;
        std::string head;
        saCsvStream(g, v, v, v, [&](const char* p, size_t n) { if (head.size() < 400) head.append(p, std::min<size_t>(n, 400)); bytes += n; for (size_t i = 0; i < n; i++) rows += p[i] == '\n'; return true; });
        CHECK(rows == g.bins + 1, "csv rows %zu", rows);
        size_t nl = head.find('\n');
        sscanf(head.c_str() + nl + 1, "%lf", &first);
        size_t nl2 = head.find('\n', nl + 1);
        sscanf(head.c_str() + nl2 + 1, "%lf", &second);
        CHECK(std::fabs((second - first) - 5e3) < 1, "csv step %.1f", second - first);
        CHECK(bytes < saCsvEstimateBytes(g.bins), "estimate %zu vs %zu", bytes, saCsvEstimateBytes(g.bins));
        SaGrid sm = saMakeGrid(100e6, 101e6, 1e4);
        std::vector<float> w(sm.bins, -40.f);
        CHECK(saCsv(sm, w, w, w).size() > 100 && saCsv(sm, w, w, w).find("frequency_hz") == 0, "saCsv");
    }
    {   // stitching: two steps of 10 MHz at 10 Msps, flat spectra at different levels, the edge between them
        SaGrid g = saMakeGrid(100e6, 116e6, 1e5);
        SaStitcher st; st.setGrid(g);
        const size_t n = 1024;
        std::vector<float> a(n, 1e-3f), b(n, 1e-5f);
        st.add(104e6, 10e6, a.data(), n, 4e6, 0);   // covers 100 .. 108
        st.add(112e6, 10e6, b.data(), n, 4e6, 0);   // covers 108 .. 116
        auto db = st.finishDb();
        CHECK(st.covered() == g.bins, "covered %zu of %zu", st.covered(), g.bins);
        CHECK(std::fabs(db[(size_t)g.index(107.95e6)] + 30) < 0.01 && std::fabs(db[(size_t)g.index(108.05e6)] + 50) < 0.01, "edge %.2f %.2f",
              db[(size_t)g.index(107.95e6)], db[(size_t)g.index(108.05e6)]);
        // a tone in one FFT bin lands in the right grid bin
        std::vector<float> c(n, 1e-6f);
        const double fb = 10e6 / n;
        const size_t ti = (size_t)std::lround((102.3e6 - (104e6 - 5e6)) / fb);
        c[ti] = 1.f;
        st.beginSweep();
        st.add(104e6, 10e6, c.data(), n, 4e6, 0);
        db = st.finishDb();
        size_t pk = 0; for (size_t i = 0; i < db.size(); i++) if (db[i] > db[pk]) pk = i;
        const double fTone = 104e6 - 5e6 + (ti + 0.5) * fb;
        CHECK((long)pk == g.index(fTone), "tone bin %zu want %ld", pk, g.index(fTone));
        CHECK(db[(size_t)g.index(110e6)] <= -199, "uncovered bin");
        // DC gap
        st.beginSweep(); st.add(104e6, 10e6, a.data(), n, 4e6, 1e6);
        db = st.finishDb();
        CHECK(db[(size_t)g.index(104.02e6)] <= -199 && db[(size_t)g.index(102e6)] > -31, "dc gap");
        // a FFT coarser than the grid: every grid bin still filled
        SaGrid fine = saMakeGrid(100e6, 108e6, 1e3);
        SaStitcher s2; s2.setGrid(fine);
        std::vector<float> d(64, 1e-4f);
        s2.add(104e6, 10e6, d.data(), 64, 4e6, 0);
        CHECK(s2.covered() == fine.bins, "coarse fft covered %zu / %zu", s2.covered(), fine.bins);
    }
    {   // spurs
        SaGrid g = saMakeGrid(0, 100e6, 1e5);
        std::vector<float> db(g.bins, -80.f);
        db[(size_t)g.index(50e6)] = -20; db[(size_t)g.index(50.1e6)] = -40;
        db[(size_t)g.index(70e6)] = -30;   // a real signal away from the step centres
        const int ch = saRemoveSpurs(db, g, {50e6}, 2e5);
        CHECK(ch == 2 && db[(size_t)g.index(50e6)] == -80 && db[(size_t)g.index(70e6)] == -30, "spurs %d", ch);
    }
    {   // traces
        SaTraces t; t.avgN = 4;
        t.update({-50, -60});
        t.update({-40, -70});
        CHECK(t.peak[0] == -40 && t.peak[1] == -60 && t.live[0] == -40, "peak");
        const double want = 10 * std::log10((1e-5 + 1e-4) / 2);   // two sweeps: plain mean of the power
        CHECK(std::fabs(t.avg[0] - want) < 0.01, "avg %.3f want %.3f", t.avg[0], want);
        for (int i = 0; i < 200; i++) t.update({-30, -30});
        CHECK(std::fabs(t.avg[0] + 30) < 0.01 && t.peak[0] == -30, "avg converges %.3f", t.avg[0]);
        t.resetPeak(); t.update({-60, -60});
        CHECK(t.peak[0] == -30, "peak after reset %.1f", t.peak[0]);
        t.update({-60});   // size change: start again
        CHECK(t.sweeps == 1 && t.peak.size() == 1, "resize");
    }
    {   // CSV
        SaGrid g = saMakeGrid(100e6, 100.3e6, 1e5);
        const std::string s = saCsv(g, {-10.5f, -20, -200}, {-5, -6, -7}, {});
        CHECK(s == "frequency_hz,live_dbfs,peak_dbfs,average_dbfs\n100050000,-10.50,-5.00,\n100150000,-20.00,-6.00,\n100250000,,-7.00,\n", "csv:\n%s", s.c_str());
    }
    {   // allocations, presets, ticks
        const SaBand* b = saAllocationAt(98.0); CHECK(b && std::string(b->name) == "FM", "fm");
        b = saAllocationAt(433.92); CHECK(b && std::string(b->name) == "ISM 433", "433 (narrowest)");
        b = saAllocationAt(1090); CHECK(b && std::string(b->name) == "ADS-B", "adsb");
        CHECK(!saAllocationAt(4500), "none");
        int n = 0; const SaBand* all = saAllocations(n);
        for (int i = 1; i < n; i++) CHECK(all[i].loMhz >= all[i - 1].loMhz && all[i].hiMhz > all[i].loMhz, "sorted %d", i);
        int np = 0; const SaPreset* p = saPresets(np);
        CHECK(np >= 12 && p[0].loMhz == 1 && p[0].hiMhz == 6000, "presets");
        CHECK(saTickStep(0, 6000, 10) == 1000 && saTickStep(87.5, 108, 10) == 5 && std::fabs(saTickStep(433, 435, 8) - 0.5) < 1e-9, "ticks");
    }
    {   // stepped receiver: a made-up radio at 2 Msps with tones at 101.3 and 104.2 MHz, swept 100 .. 106 MHz
        SpecanReceiver rx;
        double radioHz = 0;
        int retunes = 0;
        rx.configure(2e6);
        rx.setBinHz(10e3);
        rx.setRange(100e6, 106e6);
        rx.setRetuneCallback([&](double hz) { radioHz = hz; retunes++; return true; });
        std::mt19937 rng(3);
        dect2::normal_distribution<float> g(0, 0.003f);
        std::vector<cf32> blk(8192);
        double ph1 = 0, ph2 = 0;
        SpecanTelemetry t;
        bool got = false;
        for (int it = 0; it < 4000 && !got; it++) {
            for (auto& v : blk) {
                v = cf32{g(rng), g(rng)} + cf32(0.05f, 0);   // a DC spike at every centre
                if (std::fabs(101.3e6 - radioHz) < 1e6) { ph1 += 2 * M_PI * (101.3e6 - radioHz) / 2e6; v += 0.1f * cf32((float)std::cos(ph1), (float)std::sin(ph1)); }
                if (std::fabs(104.2e6 - radioHz) < 1e6) { ph2 += 2 * M_PI * (104.2e6 - radioHz) / 2e6; v += 0.01f * cf32((float)std::cos(ph2), (float)std::sin(ph2)); }
            }
            rx.feed(blk.data(), blk.size());
            got = rx.telemetry(t, 0) && t.blocksOk >= 2;
        }
        CHECK(got, "no sweep");
        if (got) {
            SaGrid gr = saMakeGrid(t.loHz, t.loHz + t.binHz * t.live.size(), t.binHz);
            auto at = [&](double hz) { return t.live[(size_t)gr.index(hz)]; };
            printf("  stepped: %d steps, %zu bins, 101.3 %.1f, 104.2 %.1f, floor %.1f, step centre %.1f\n", t.steps, t.live.size(), at(101.3e6), at(104.2e6), at(102.2e6), t.stepLos.empty() ? 0 : at(t.stepLos[1]));
            CHECK(t.steps == 4 && !t.native && retunes >= 8, "steps %d retunes %d", t.steps, retunes);
            CHECK(std::fabs(at(101.3e6) + 20) < 3 && std::fabs(at(104.2e6) + 40) < 3, "tone levels");
            CHECK(at(102.2e6) < -60, "floor %.1f", at(102.2e6));
            CHECK(at(t.stepLos[1]) < -55, "DC spike removed %.1f", at(t.stepLos[1]));
            CHECK(std::fabs(t.peakHz - 101.3e6) < 2e4, "peak at %.0f", t.peakHz);
            CHECK(t.blocksBad == 0, "gaps");
        }
    }
    printf(fails ? "specan dsp: %d FAILED\n" : "specan dsp: all passed\n", fails);
    return fails ? 1 : 0;
}
