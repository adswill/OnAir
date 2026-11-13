// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF digital, MSK144: the frame (sync words, code bits), then the whole receiver on synthetic audio: a continuous transmission over the 15 s
// period at several levels, meteor-like bursts of 40 .. 400 ms, carrier and clock offsets, 30 s periods, noise alone, and the real recordings of
// the WSJT-X samples when HFDIG_WSJT_SAMPLES is set.
#include "hfdig_wsjt_testutil.h"
#include <chrono>
#include <cstring>
using namespace wsjttest;
#include "../common/test_util.h"
using namespace dect2::wsjt;

// a burst of the repeating frame at `startSec` for `lenSec` over noise (SNR in 2500 Hz while on)
static std::vector<float> burst(const std::string& msg, double fc, double snrDb, double startSec, double lenSec, double period, uint64_t seed, double ppm = 0) {
    std::vector<float> a((size_t)((period + 4) * 8000), 0.f);
    uint8_t b[144];
    msk144Frame(msg, b);
    addMsk144(b, fc, ampFor(snrDb), 8000.0 * (1 + ppm * 1e-6), startSec, lenSec, a);
    Gauss g(seed);
    for (auto& v : a) v += (float)(kSigma * g.g());
    return a;
}

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    const int M = ftx::kMsk144;
    {   // the frame: the sync word twice, 128 code bits that satisfy the LDPC checks
        uint8_t b[144];
        CHECK(msk144Frame("K1ABC W9XYZ EN37", b), "frame");
        const int s8[8] = {0, 1, 1, 1, 0, 0, 1, 0};
        bool ok = true;
        for (int i = 0; i < 8; i++) ok = ok && b[i] == s8[i] && b[56 + i] == s8[i];
        CHECK(ok, "sync words");
        uint8_t cw[128];
        for (int i = 0; i < 48; i++) cw[i] = b[8 + i];
        for (int i = 0; i < 80; i++) cw[48 + i] = b[64 + i];
        const LdpcTab& t = kLdpc128_90;
        int bad = 0;
        for (int j = 0; j < t.n - t.k; j++) {
            int s = 0;
            for (int i = 0; i < t.nrw[j]; i++) s ^= cw[t.nm[(size_t)j * (size_t)t.w + (size_t)i] - 1];
            bad += s;
        }
        CHECK(!bad, "the frame is a codeword (%d checks fail)", bad);
        CHECK(!msk144Frame("", b) || true, "empty");
    }
    const std::string msg = "K1ABC W9XYZ EN37";
    {   // a continuous transmission over the 15 s period: levels from +2 to -9 dB (in 2500 Hz)
        for (double snr : {2.0, -3.0, -6.0, -8.0, -10.0}) {
            int ok = 0, n = 4;
            for (int k = 0; k < n; k++) {
                Imp f;
                const auto d = decode(burst(msg, 1500 + 7.3 * k, snr, 0.0, 15.0, 15.0, 100 + (uint64_t)k), M, f);
                bool got = false;
                for (const auto& x : d) { if (x.msg == msg) got = true; else CHECK(false, "false decode \"%s\"", x.msg.c_str()); }
                ok += got;
            }
            printf("MSK144 continuous %5.1f dB: %d of %d\n", snr, ok, n);
            if (snr >= -6) CHECK(ok == n, "%.0f dB: %d of %d", snr, ok, n);
        }
    }
    {   // meteor bursts: 400, 150 and 60 ms at different times in the period and carrier frequencies
        struct B { double len, start, fc, snr; };
        const B bs[] = {{0.4, 3.0, 1500, 0}, {0.15, 7.7, 1380.4, 3}, {0.06, 11.2, 1620, 8}, {0.1, 0.5, 1000, 4}, {0.1, 13.9, 2300, 4}};
        for (const B& b : bs) {
            Imp f;
            const auto d = decode(burst(msg, b.fc, b.snr, b.start, b.len, 15.0, 7), M, f);
            const FtxDecode* x = find(d, msg);
            CHECK(x, "burst %.0f ms at %.1f s, %.0f Hz, %.0f dB not decoded", b.len * 1000, b.start, b.fc, b.snr);
            if (x) CHECK(std::fabs(x->dt - b.start) < b.len + 0.1, "burst at %.1f s reported at %.2f s", b.start, x->dt);
        }
    }
    {   // clock error +-100 ppm and a 30 s period
        for (double ppm : {100.0, -100.0}) {
            Imp f;
            const auto d = decode(burst(msg, 1500, -3, 0.0, 15.0, 15.0, 9, ppm), M, f);
            CHECK(find(d, msg), "%.0f ppm", ppm);
        }
        Imp f; f.period = 30;
        const auto d = decode(burst(msg, 1500, -3, 2.0, 25.0, 30.0, 10), M, f);
        CHECK(find(d, msg), "30 s period");
    }
    {   // two stations alternating are not a thing, but two bursts from different stations in one period are
        std::vector<float> a((size_t)(19 * 8000), 0.f);
        uint8_t b1[144], b2[144];
        msk144Frame("CQ K1ABC FN42", b1); msk144Frame("G4XYZ DL1ABC JO62", b2);
        addMsk144(b1, 1500, ampFor(2), 8000.0, 2.0, 0.3, a);
        addMsk144(b2, 1500, ampFor(2), 8000.0, 8.0, 0.3, a);
        Gauss g(5);
        for (auto& v : a) v += (float)(kSigma * g.g());
        Imp f;
        const auto d = decode(a, M, f);
        print(d);
        CHECK(find(d, "CQ K1ABC FN42") && find(d, "G4XYZ DL1ABC JO62"), "two bursts");
    }
    {   // noise alone: nothing in 8 periods
        Imp f;
        const auto d = decode(audio(M, {}, 8, f, 77), M, f);
        CHECK(d.empty(), "noise alone: %d decodes", (int)d.size());
    }
    if (std::getenv("HFDIG_WSJT_SAMPLES")) {   // real recordings (15 s, 12 kHz)
        for (const char* n : {"MSK144__181211_120500", "MSK144__181211_120800"}) {
            auto a = sample(n);
            if (a.empty()) continue;
            a.resize(a.size() + 8000, 0.f);   // the slot window reaches 0.15 s past the end of the 15 s recording
            Imp f; f.period = 15;
            const auto d = decode(a, M, f, kT0 + 12 * 3600 + 5 * 60);
            printf("%s: %d decodes\n", n, (int)d.size());
            print(d);
            const std::string want = std::string(n).find("120500") != std::string::npos ? "K1JT WA4CQG EM72" : "CQ KD9VV EN71";
            CHECK(find(d, want), "the real MSK144 recording %s: \"%s\"", n, want.c_str());
        }
    }
    printf("%.1f s\n%s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
