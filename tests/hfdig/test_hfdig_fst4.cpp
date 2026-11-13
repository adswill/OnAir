// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF digital, FST4 and FST4W: the LDPC tables (the generator and the parity-check lists of WSJT-X come from different files: every codeword must
// satisfy every check), then the whole receiver on synthetic audio (clean, near the threshold, 15 .. 300 s, frequency / time / clock offsets, two
// stations, noise alone) and the real recordings of the WSJT-X samples when HFDIG_WSJT_SAMPLES is set.
#include "hfdig_wsjt_testutil.h"
#include <chrono>
#include <cstring>
using namespace wsjttest;
#include "../common/test_util.h"
using namespace dect2::wsjt;

static bool checksOk(const LdpcTab& t, const uint8_t* cw) {
    const int m = t.n - t.k;
    for (int j = 0; j < m; j++) {
        int s = 0;
        for (int i = 0; i < t.nrw[j]; i++) s ^= cw[t.nm[(size_t)j * (size_t)t.w + (size_t)i] - 1];
        if (s) return false;
    }
    return true;
}

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    {   // every codeword of the three codes satisfies every check of its parity-check list; BP and OSD decode a noisy codeword
        Gauss g(3);
        for (const LdpcTab* t : {&kLdpc240_101, &kLdpc240_74, &kLdpc128_90}) {
            int bad = 0, bpOk = 0, osdOk = 0;
            for (int trial = 0; trial < 40; trial++) {
                uint8_t msg[128], cw[256], dec[256];
                for (int i = 0; i < t->k; i++) msg[i] = (uint8_t)(g.u() < 0.5);
                ldpcEncode(*t, msg, cw);
                if (!checksOk(*t, cw)) bad++;
                // the bits with Gaussian noise at Eb/N0 of about 2.5 dB: BP should mostly get it, OSD should
                std::vector<float> llr((size_t)t->n);
                const double sigma = 0.62;
                for (int i = 0; i < t->n; i++) llr[(size_t)i] = (float)(2.0 * ((cw[i] ? 1.0 : -1.0) + sigma * g.g()) / (sigma * sigma));
                if (ldpcBp(*t, llr.data(), 60, dec) == 0 && !memcmp(dec, cw, (size_t)t->n)) bpOk++;
                struct C { const uint8_t* want; int n; } cc{cw, t->n};
                if (ldpcOsd(*t, llr.data(), 2, dec, [](const uint8_t* c, void* x) { auto* q = (C*)x; return !memcmp(c, q->want, (size_t)q->n); }, &cc)) osdOk++;
            }
            CHECK(!bad, "LDPC (%d,%d): %d codewords break a check", t->n, t->k, bad);
            printf("LDPC (%d,%d): BP %d / 40, OSD-2 %d / 40\n", t->n, t->k, bpOk, osdOk);
            CHECK(osdOk >= 30, "LDPC (%d,%d): OSD %d of 40", t->n, t->k, osdOk);
        }
    }
    {   // tones: 160, 40 sync symbols in the right places
        const auto t = fst4Tones("CQ K1ABC FN42", false);
        CHECK(t.size() == 160, "160 tones");
        const int w1[8] = {0, 1, 3, 2, 1, 0, 2, 3};
        bool ok = true;
        for (int k = 0; k < 8; k++) ok = ok && t[(size_t)k] == w1[k] && t[(size_t)(76 + k)] == w1[k] && t[(size_t)(152 + k)] == w1[k];
        CHECK(ok, "sync word 1");
        CHECK(fst4Tones("K1ABC FN42 37", true).size() == 160, "FST4W tones");
        CHECK(fst4Tones("K1ABC FN42 37", true) != fst4Tones("K1ABC FN42 37", false), "FST4W differs from FST4");
    }
    const int M = ftx::kFst4, MW = ftx::kFst4w;
    const std::vector<St> one = {{"CQ K1ABC FN42", 1500, -10, 0.0}};
    {   // clean, every period
        for (double P : {15.0, 30.0, 60.0, 300.0}) {
            Imp f; f.period = P;
            const auto d = decode(audio(M, one, 1, f, 3), M, f);
            const FtxDecode* x = find(d, "CQ K1ABC FN42");
            CHECK(x && std::fabs(x->dt) < 0.1 && std::fabs(x->hz - 1500) < 0.5, "clean FST4-%.0f", P);
            if (x) CHECK(std::fabs(x->snrDb + 10) < 3.0, "FST4-%.0f SNR estimate %.1f dB against -10", P, x->snrDb);
        }
    }
    {   // FST4W
        Imp f; f.period = 120;
        const auto d = decode(audio(MW, {{"K1ABC FN42 37", 1400, -12, 0.2}, {"G4XYZ IO91 23", 1700, -15, -0.4}}, 1, f, 4), MW, f);
        print(d);
        CHECK(find(d, "K1ABC FN42 37") && find(d, "G4XYZ IO91 23"), "FST4W-120");
    }
    {   // threshold sweep of FST4-60 and FST4-15 (SNR in 2500 Hz)
        for (double P : {60.0, 15.0}) {
            const double base = P == 60 ? -25.0 : -18.0;
            for (double snr : {base + 6, base + 2, base, base - 3}) {
                Imp f; f.period = P;
                int ok = 0, n = 4;
                for (int k = 0; k < n; k++) {
                    const auto d = decode(audio(M, {{"CQ K1ABC FN42", 1000 + 37.3 * k, snr, 0.1 * k}}, 1, f, 100 + k), M, f);
                    if (find(d, "CQ K1ABC FN42")) ok++;
                    for (const auto& x : d) CHECK(x.msg == "CQ K1ABC FN42", "false decode \"%s\"", x.msg.c_str());
                }
                printf("FST4-%.0f %5.1f dB: %d of %d\n", P, snr, ok, n);
                if (snr >= base + 2) CHECK(ok == n, "FST4-%.0f %.1f dB: %d of %d", P, snr, ok, n);
            }
        }
    }
    {   // offsets: frequency (also fractions of the tone spacing), time, clock, dial
        struct C { double hz, dt, ppm, dial; };
        const C cs[] = {{700.0, -2.4, 0, 0}, {701.3, 2.4, 0, 0}, {1802.7, 0.5, 0, 0}, {2700.4, 0.0, 0, 0}, {1500, 0.3, 100, 0}, {1500, 0.3, -100, 0},
                        {1500, -1.1, 0, 100}, {1500, 1.7, 0, -100}, {250.0, 0.2, 0, 0}, {1500.0, 0.123, 0, 0}};
        for (const C& c : cs) {
            Imp f; f.ppm = c.ppm; f.dialHz = c.dial;
            const auto d = decode(audio(M, {{"W9XYZ K1ABC EN37", c.hz, -20, c.dt}}, 1, f, 9), M, f);
            const FtxDecode* x = find(d, "W9XYZ K1ABC EN37");
            CHECK(x && std::fabs(x->dt - c.dt) < 0.1, "f %.1f dt %.1f ppm %.0f dial %.0f: %s", c.hz, c.dt, c.ppm, c.dial, x ? "wrong DT" : "not decoded");
        }
    }
    {   // Doppler drift: all frequencies change in proportion (0.15 Hz/s at 1500 Hz, 7 Hz over the transmission), both ways, and a stronger one (0.2)
        for (double dr : {0.15, -0.15, 0.2}) {
            Imp f; f.drift = dr;
            const auto d = decode(audio(M, {{"W9XYZ K1ABC EN37", 1500, -22, 0.2}}, 1, f, 21), M, f);
            CHECK(find(d, "W9XYZ K1ABC EN37"), "drift %.2f Hz/s: not decoded", dr);
        }
    }
    {   // two stations 40 Hz apart and a third far away
        Imp f;
        const std::vector<St> st = {{"CQ K1ABC FN42", 1000, -14, 0.0}, {"K1ABC W9XYZ -12", 1040, -14, 0.5}, {"CQ DX G4ABC IO91", 2300, -18, -0.7}};
        const auto d = decode(audio(M, st, 1, f, 12), M, f);
        print(d);
        for (const auto& s : st) CHECK(find(d, s.msg), "\"%s\" not decoded", s.msg.c_str());
    }
    {   // noise alone: no decodes (4 slots of FST4-60, 2 of FST4W-120)
        Imp f;
        const auto d = decode(audio(M, {}, 4, f, 77), M, f);
        CHECK(d.empty(), "noise alone: %d decodes", (int)d.size());
        Imp g; g.period = 120;
        const auto dw = decode(audio(MW, {}, 2, g, 78), MW, g);
        CHECK(dw.empty(), "noise alone (FST4W): %d decodes", (int)dw.size());
    }
    if (std::getenv("HFDIG_WSJT_SAMPLES")) {   // real recordings of the WSJT-X samples
        for (const char* n : {"FST4+FST4W__210115_0058"}) {
            const auto a = sample(n);
            if (a.empty()) continue;
            for (double P : {60.0, 120.0}) {
                for (int w = 0; w < 2; w++) {
                    Imp f; f.period = P;
                    const auto d = decode(a, w ? MW : M, f, kT0 + 58 * 60);   // 00:58
                    printf("%s FST4%s-%.0f: %d decodes\n", n, w ? "W" : "", P, (int)d.size());
                    print(d);
                    if (!w && P == 60) CHECK(find(d, "CQ K9KFR EN71") && find(d, "CQ N5TM EL29"), "the real FST4-60 recording of the WSJT-X samples");
                }
            }
        }
    }
    printf("%.1f s\n%s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
