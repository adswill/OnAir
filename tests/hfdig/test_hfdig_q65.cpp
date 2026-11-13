// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF digital, Q65: the code (encoder, belief propagation over GF(64)), then the whole receiver on synthetic audio (every period and sub-mode, the
// threshold, frequency / time / clock offsets, two stations, noise alone) and the real recordings of the WSJT-X samples when HFDIG_WSJT_SAMPLES is set.
#include "hfdig_wsjt_testutil.h"
#include <chrono>
#include <cstring>
using namespace wsjttest;
#include "../common/test_util.h"
using namespace dect2::wsjt;

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    const int M = ftx::kQ65;
    {   // the code: random messages encode, and decode from clean and from noisy tone energies
        Gauss g(5);
        int okClean = 0, okNoisy = 0;
        for (int trial = 0; trial < 30; trial++) {
            int d[13], cw[63];
            for (int i = 0; i < 13; i++) d[i] = (int)(g.u() * 64);
            d[12] &= ~1;
            q65Encode(d, cw);
            std::vector<float> E(63 * 64), En(63 * 64);
            for (int k = 0; k < 63; k++)
                for (int s = 0; s < 64; s++) {
                    // unit noise power, the tone amplitude 2.4: Es/N0 about 8 dB per symbol
                    const double nr = g.g(), ni = g.g(), a = s == cw[k] ? 2.4 : 0;
                    E[(size_t)k * 64 + (size_t)s] = (float)(s == cw[k] ? 100 : 0.01);
                    En[(size_t)k * 64 + (size_t)s] = (float)((a + nr) * (a + nr) + ni * ni);
                }
            int m[13];
            float llh;
            if (q65Decode(E.data(), m, 100, &llh) == 0 && !memcmp(m, d, sizeof m)) okClean++;
            if (q65Decode(En.data(), m, 100, &llh) == 0 && !memcmp(m, d, sizeof m)) okNoisy++;
        }
        printf("Q65 code: %d / 30 clean, %d / 30 noisy (Es/N0 about 8 dB)\n", okClean, okNoisy);
        CHECK(okClean == 30, "clean energies: %d of 30", okClean);
        CHECK(okNoisy >= 20, "noisy energies: %d of 30", okNoisy);
        const auto t = q65Tones("CQ K1ABC FN42");
        CHECK(t.size() == 85, "85 tones");
        int sync = 0;
        for (int v : t) { CHECK(v >= 0 && v <= 64, "tone range"); sync += v == 0; }
        CHECK(sync == 22 && t[0] == 0 && t[84] == 0 && t[8] == 0, "%d sync tones", sync);
    }
    {   // clean, every period (A) and every sub-mode (60 s)
        for (double P : {15.0, 30.0, 60.0, 120.0}) {
            Imp f; f.period = P;
            const auto d = decode(audio(M, {{"CQ K1ABC FN42", 1500, -10, 0.0}}, 1, f, 3), M, f);
            const FtxDecode* x = find(d, "CQ K1ABC FN42");
            CHECK(x && std::fabs(x->dt) < 0.15 && std::fabs(x->hz - 1500) < 1.0, "clean Q65-%.0fA", P);
            if (x) CHECK(std::fabs(x->snrDb + 10) < 3.5, "Q65-%.0fA SNR estimate %.1f dB against -10", P, x->snrDb);
        }
        for (int sub = 1; sub < 4; sub++) {
            Imp f; f.sub = sub; f.period = 60;
            const auto d = decode(audio(M, {{"K1ABC W9XYZ -15", 900, -12, 0.3}}, 1, f, 4), M, f);
            CHECK(find(d, "K1ABC W9XYZ -15"), "Q65-60%c", 'A' + sub);
        }
    }
    {   // threshold sweep of Q65-60A (SNR in 2500 Hz)
        for (double snr : {-20.0, -24.0, -26.0, -28.0}) {
            Imp f; f.period = 60;
            int ok = 0, n = 4;
            for (int k = 0; k < n; k++) {
                const auto d = decode(audio(M, {{"CQ K1ABC FN42", 1000 + 37.3 * k, snr, 0.1 * k}}, 1, f, 100 + k), M, f);
                if (find(d, "CQ K1ABC FN42")) ok++;
                for (const auto& x : d) CHECK(x.msg == "CQ K1ABC FN42", "false decode \"%s\"", x.msg.c_str());
            }
            printf("Q65-60A %5.1f dB: %d of %d\n", snr, ok, n);
            if (snr >= -24) CHECK(ok == n, "%.1f dB: %d of %d", snr, ok, n);
        }
    }
    {   // offsets: frequency, time, clock, dial
        struct C { double hz, dt, ppm, dial; };
        const C cs[] = {{700.0, -2.4, 0, 0}, {701.3, 2.4, 0, 0}, {2000.7, 0.5, 0, 0}, {1500, 0.3, 100, 0}, {1500, 0.3, -100, 0}, {1500, -1.1, 0, 100}, {250.0, 0.2, 0, 0}};
        for (const C& c : cs) {
            Imp f; f.ppm = c.ppm; f.dialHz = c.dial; f.period = 60;
            const auto d = decode(audio(M, {{"W9XYZ K1ABC EN37", c.hz, -22, c.dt}}, 1, f, 9), M, f);
            const FtxDecode* x = find(d, "W9XYZ K1ABC EN37");
            CHECK(x && std::fabs(x->dt - c.dt) < 0.15, "f %.1f dt %.1f ppm %.0f dial %.0f: %s", c.hz, c.dt, c.ppm, c.dial, x ? "wrong DT" : "not decoded");
        }
    }
    {   // Doppler drift: all frequencies change in proportion (0.15 Hz/s at 1500 Hz, 7 Hz over the transmission), both ways, and a stronger one (0.2)
        for (double dr : {0.15, -0.15, 0.2}) {
            Imp f; f.period = 60; f.drift = dr;
            const auto d = decode(audio(M, {{"W9XYZ K1ABC EN37", 1500, -22, 0.2}}, 1, f, 21), M, f);
            CHECK(find(d, "W9XYZ K1ABC EN37"), "drift %.2f Hz/s: not decoded", dr);
        }
    }
    {   // two stations 40 Hz apart and a third far away
        Imp f; f.period = 60;
        const std::vector<St> st = {{"CQ K1ABC FN42", 1000, -18, 0.0}, {"K1ABC W9XYZ -12", 1040, -18, 0.5}, {"CQ DX G4ABC IO91", 2300, -20, -0.7}};
        const auto d = decode(audio(M, st, 1, f, 12), M, f);
        print(d);
        for (const auto& s : st) CHECK(find(d, s.msg), "\"%s\" not decoded", s.msg.c_str());
    }
    {   // noise alone
        Imp f; f.period = 60;
        const auto d = decode(audio(M, {}, 4, f, 77), M, f);
        CHECK(d.empty(), "noise alone: %d decodes", (int)d.size());
    }
    if (std::getenv("HFDIG_WSJT_SAMPLES")) {   // real recordings: file, period, sub-mode, UTC of the slot (hhmm)
        struct R { const char* n; double P; int sub; };
        const R rs[] = {{"Q65__60A_EME_6m__210106_1621", 60, 0}, {"Q65__60B_1296_Troposcatter__210109_0007", 60, 1}, {"Q65__60B_1296_Troposcatter__210109_0147", 60, 1},
                        {"Q65__60B_1296_Troposcatter__210109_0151", 60, 1}, {"Q65__60D_EME_10GHz__201212_1838", 60, 3}, {"Q65__30A_Ionoscatter_6m__201203_022700", 30, 0},
                        {"Q65__30A_Ionoscatter_6m__201203_022800", 30, 0}, {"Q65__30A_Ionoscatter_6m__201203_022900", 30, 0}, {"Q65__30A_Ionoscatter_6m__201203_024000", 30, 0},
                        {"Q65__120E_Ionoscatter_6m__210130_1438", 120, 4}, {"Q65__120E_Ionoscatter_6m__210130_1442", 120, 4}, {"Q65__120D_Rainscatter_10_GHz__210117_0920", 120, 3}};
        int total = 0;
        for (const R& r : rs) {
            const auto a = sample(r.n);
            if (a.empty()) continue;
            Imp f; f.period = r.P; f.sub = r.sub;
            const auto d = decode(a, M, f, kT0);
            printf("%s (Q65-%.0f%c): %d decodes\n", r.n, r.P, 'A' + r.sub, (int)d.size());
            print(d);
            total += (int)d.size();
            if (std::string(r.n).find("60A_EME") != std::string::npos)
                CHECK(find(d, "W7GJ VE1JF RRR") && find(d, "W7GJ W1VD FN31") && find(d, "W7GJ N0TB -15") && find(d, "W7GJ N8JX EN73"), "the real Q65-60A EME recording");
        }
        printf("real Q65 recordings: %d decodes\n", total);
    }
    printf("%.1f s\n%s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
