// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF digital, JT9: the code (sync, convolutional coding, Fano), then the whole receiver on synthetic audio
// (clean, near the threshold, A / B / C, frequency and time offsets, clock error, two stations, noise alone) and on the real recordings of the
// WSJT-X samples when HFDIG_WSJT_SAMPLES is set.
#include "hfdig_wsjt_testutil.h"
#include "../../core/src/hfdig/hfdig_wsjt_int.h"
#include <cstring>
#include <chrono>
using namespace wsjttest;
#include "../common/test_util.h"
using namespace dect2::wsjt;

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    const int M = ftx::kJt9;
    {   // the code: a message encodes to 85 tones of which 16 are sync (tone 0) and 69 data (1 .. 8); the convolutional code is the one of WSPR
        const auto t = jt9Tones("CQ K1ABC FN42");
        CHECK(t.size() == 85, "85 tones");
        int sync = 0;
        for (int v : t) { CHECK(v >= 0 && v <= 8, "tone range"); sync += v == 0; }
        CHECK(sync == 16, "%d sync tones", sync);
        CHECK(jt9Tones("K1ABC W9XYZ -15") != t, "different messages differ");
        // convEncode32 and fano32: round trip with noise-free soft bits and with 20 % of the bits flipped
        uint8_t d[13] = {0x5A, 0xC3, 0x17, 0x99, 0x01, 0xFE, 0x42, 0x88, 0x10}, c[206], d2[13];
        convEncode32(d, 72, c);
        float llr[206];
        for (int i = 0; i < 206; i++) llr[i] = c[i] ? 4.f : -4.f;
        CHECK(fano32(llr, 72, d2, 400000) && !memcmp(d, d2, 9), "Fano, clean");
        Gauss g(5);
        for (int i = 0; i < 206; i++) llr[i] = (c[i] ? 1.f : -1.f) * 1.0f + (float)(1.0 * g.g());
        CHECK(fano32(llr, 72, d2, 4000000) && !memcmp(d, d2, 9), "Fano, noisy (Es/N0 about 0 dB)");
    }
    const std::vector<St> one = {{"CQ K1ABC FN42", 1500, -10, 0.0}};
    {   // clean, strong
        Imp f;
        const auto d = decode(audio(M, one, 2, f, 3), M, f);
        print(d);
        const FtxDecode* x = find(d, "CQ K1ABC FN42");
        CHECK(x && std::fabs(x->hz - 1500) < 0.5 && std::fabs(x->dt) < 0.1, "clean JT9A");
        CHECK(d.size() == 2, "clean: %d decodes (2 slots)", (int)d.size());
        if (x) CHECK(std::fabs(x->snrDb + 10) < 2.5, "SNR estimate %.1f dB against -10", x->snrDb);
    }
    {   // threshold sweep: how many of 6 slots decode at each level (SNR in 2500 Hz, noise only changes with the seed)
        for (double snr : {-18.0, -22.0, -24.0, -25.0, -26.0}) {
            Imp f;
            int ok = 0, n = 6;
            for (int k = 0; k < n; k++) {
                const auto d = decode(audio(M, {{"CQ K1ABC FN42", 1000 + 37.3 * k, snr, 0.1 * k}}, 1, f, 100 + k), M, f);
                if (find(d, "CQ K1ABC FN42")) ok++;
                for (const auto& x : d) CHECK(x.msg == "CQ K1ABC FN42", "false decode \"%s\" at %.0f dB", x.msg.c_str(), snr);
            }
            printf("JT9A %5.1f dB: %d of %d\n", snr, ok, n);
            if (snr >= -22) CHECK(ok == n, "%.0f dB: %d of %d", snr, ok, n);
            if (snr == -24) CHECK(ok >= 4, "%.0f dB: %d of %d", snr, ok, n);
        }
    }
    {   // frequency offsets (fractions of a tone spacing and whole tones), time offsets, clock error, dial error
        struct C { double hz, dt, ppm, dial; };
        const C cs[] = {{700.0, -2.4, 0, 0}, {701.3, 2.4, 0, 0}, {1802.7, 0.5, 0, 0}, {2700.4, 0.0, 0, 0}, {1500, 0.3, 100, 0}, {1500, 0.3, -100, 0},
                        {1500, -1.1, 0, 100}, {1500, 1.7, 0, -100}, {250.0, 0.2, 0, 0}};
        for (const C& c : cs) {
            Imp f; f.ppm = c.ppm; f.dialHz = c.dial;
            const auto d = decode(audio(M, {{"W9XYZ K1ABC EN37", c.hz, -19, c.dt}}, 1, f, 9), M, f);
            const FtxDecode* x = find(d, "W9XYZ K1ABC EN37");
            CHECK(x && std::fabs(x->dt - c.dt) < 0.15, "f %.1f dt %.1f ppm %.0f dial %.0f: %s", c.hz, c.dt, c.ppm, c.dial, x ? "wrong DT" : "not decoded");
        }
    }
    {   // Doppler drift: all frequencies change in proportion (0.15 Hz/s at 1500 Hz, 7 Hz over the transmission), both ways, and a stronger one (0.2)
        for (double dr : {0.15, -0.15, 0.17}) {
            Imp f; f.drift = dr;
            const auto d = decode(audio(M, {{"W9XYZ K1ABC EN37", 1500, -20, 0.2}}, 1, f, 21), M, f);
            CHECK(find(d, "W9XYZ K1ABC EN37"), "drift %.2f Hz/s: not decoded", dr);
        }
    }
    {   // two stations, 40 Hz apart, and a third at the far end
        Imp f;
        const std::vector<St> st = {{"CQ K1ABC FN42", 1000, -16, 0.0}, {"K1ABC W9XYZ -12", 1090, -14, 0.5}, {"CQ DX G4ABC IO91", 2300, -18, -0.7}};
        const auto d = decode(audio(M, st, 1, f, 12), M, f);
        print(d);
        for (const auto& s : st) CHECK(find(d, s.msg), "\"%s\" not decoded", s.msg.c_str());
    }
    {   // noise alone: no decodes (5 slots)
        Imp f;
        const auto d = decode(audio(M, {}, 5, f, 77), M, f);
        CHECK(d.empty(), "noise alone: %d decodes", (int)d.size());
    }
    if (const char* dir = std::getenv("HFDIG_WSJT_SAMPLES")) {   // real recordings of the WSJT-X samples (JT9, 12 kHz)
        (void)dir;
        int total = 0;
        for (const char* n : {"JT9__130418_1742"}) {
            const auto a = sample(n);
            if (a.empty()) continue;
            Imp f;
            HfdigFtxTelemetry tel;
            const auto d = decode(a, M, f, kT0, &tel);
            printf("%s: %d decodes\n", n, (int)d.size());
            print(d);
            total += (int)d.size();
            CHECK(find(d, "K1JT KF4RWA 73") && find(d, "CQ M0WAY IO82") && find(d, "G7CNF N4HFA EL89"), "the real JT9 recording of the WSJT-X samples (130418_1742)");
        }
        printf("real JT9 recordings: %d decodes\n", total);
    }
    printf("%.1f s\n%s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
