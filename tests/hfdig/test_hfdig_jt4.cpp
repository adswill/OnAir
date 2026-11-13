// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF digital, JT4: the code (206 tones of 4, the timing), then the whole receiver on synthetic audio (clean, sub-modes A .. G, frequency and
// time offsets, a weak signal, two stations, noise alone). The synthetic audio is loopback, but the encoder was checked against WSJT-X (jt9 -4 decodes
// it) and the receiver against the real WSJT-X sample DF2ZC_070926_040700.WAV (JT4A, "K1JT DF2ZC JO30", sync bit inverted, 5.1 s late).
#include "hfdig_wsjt_testutil.h"
#include "../../core/src/hfdig/hfdig_wsjt_int.h"
#include <chrono>
using namespace wsjttest;
#include "../common/test_util.h"
using namespace dect2::wsjt;

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    const int M = ftx::kJt4;
    {
        const auto t = jt4Tones("CQ K1ABC FN42");
        CHECK(t.size() == 206, "206 tones");
        for (int v : t) CHECK(v >= 0 && v <= 3, "tone range");
        CHECK(jt4Tones("K1ABC W9XYZ -15") != t, "different messages differ");
        CHECK(std::fabs(ftxPeriod(M) - 60.0) < 1e-9 && std::fabs(206 * 2520.0 / 11025 - 47.088) < 0.01, "47.1 s transmission");
    }
    {   // clean, sub-mode A, with a fractional-bin frequency offset
        Imp f;
        const auto d = decode(audio(M, {{"CQ K1ABC FN42", 1501.7, -10, 0.2}}, 2, f, 3), M, f);
        print(d);
        const FtxDecode* x = find(d, "CQ K1ABC FN42");
        CHECK(x && std::fabs(x->hz - 1501.7) < 2.5 && std::fabs(x->dt - 0.2) < 0.1, "clean JT4A");
        CHECK(d.size() == 2, "clean: %d decodes (2 slots)", (int)d.size());
    }
    for (int sub = 0; sub < 7; sub++) {   // the sub-modes, near the threshold, odd offsets
        Imp f; f.sub = sub;
        const auto d = decode(audio(M, {{"K1ABC W9XYZ -12", 900.0 + 13.3 * sub, -16, -0.8 + 0.3 * sub}}, 1, f, 40 + sub), M, f);
        CHECK(find(d, "K1ABC W9XYZ -12"), "JT4%c at -16 dB: not decoded", 'A' + sub);
    }
    {   // a weak one
        Imp f;
        int ok = 0;
        for (int k = 0; k < 4; k++) {
            const auto d = decode(audio(M, {{"CQ DX G4ABC IO91", 1200 + 31.1 * k, -19, 0.1 * k}}, 1, f, 200 + k), M, f);
            if (find(d, "CQ DX G4ABC IO91")) ok++;
            for (const auto& x : d) CHECK(x.msg == "CQ DX G4ABC IO91", "false decode \"%s\"", x.msg.c_str());
        }
        printf("JT4A -19 dB: %d of 4\n", ok);
        CHECK(ok >= 2, "-19 dB: %d of 4", ok);
    }
    {   // two stations
        Imp f;
        const std::vector<St> st = {{"CQ K1ABC FN42", 1000, -13, 0.0}, {"K1ABC W9XYZ -12", 1300, -13, 0.5}};
        const auto d = decode(audio(M, st, 1, f, 12), M, f);
        for (const auto& s : st) CHECK(find(d, s.msg), "\"%s\" not decoded", s.msg.c_str());
    }
    {   // the sync bit sent inverted and a late start (5.05 s into the minute): the shape of the WSJT-X sample DF2ZC_070926_040700.WAV,
        // which decodes as "K1JT DF2ZC JO30" (about -22 dB, JT4A, lowest tone 1250.5 Hz) in jt9 and in this receiver (checked by hand, 1.1 MB: not committed)
        Imp f;
        auto tones = jt4Tones("K1JT DF2ZC JO30");
        for (int& v : tones) v ^= 1;
        std::vector<float> a((size_t)(64 * 8000), 0.f);
        addJt4(tones, 0, 1250.4, ampFor(-17), 8000.0, 5.05, a);
        Gauss g(5);
        for (auto& v : a) v += (float)(kSigma * g.g());
        const auto d = decode(a, M, f);
        print(d);
        const FtxDecode* x = find(d, "K1JT DF2ZC JO30");
        CHECK(x && std::fabs(x->hz - 1250.4) < 2.5, "inverted sync, late start");
    }
    {   // noise alone
        Imp f;
        const auto d = decode(audio(M, {}, 5, f, 77), M, f);
        CHECK(d.empty(), "noise alone: %d decodes", (int)d.size());
    }
    printf("%.1f s\n%s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
