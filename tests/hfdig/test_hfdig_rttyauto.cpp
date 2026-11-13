// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RTTY auto-lock (rttyAutoAnalyse and HfdigRtty::autoLock): speed, shift and polarity from the audio alone. Synthetic transmissions of every speed
// (45.45 .. 100 baud) and shift (170 .. 850 Hz), both polarities, tones anywhere in the passband, noise down to a few dB, the lines a little off
// the table's shifts; noise alone and a CW-like tone must give nothing. With "--sweep" the program prints the table only.
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/hfdig/hfdig_rtty.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

namespace {
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 2654435761ull + 7) {}
    double u() { s = s * 6364136223846793005ull + 1442695040888963407ull; return ((s >> 11) + 0.5) / 9007199254740992.0; }
    double g() { return std::sqrt(-2 * std::log(u())) * std::cos(6.283185307179586 * u()); }
};

// seconds of RTTY audio at the given SNR (tone power against the noise in 3 kHz; the generator's tone has amplitude 0.5)
std::vector<float> rtty(int baud, int shift, bool rev, double low, double snrDb, double seconds, uint64_t seed) {
    auto gen = makeRttyTxAudio("", baud, shift, rev, low);
    std::vector<float> a((size_t)(seconds * 8000));
    gen->generate(a.data(), a.size());
    const double sigma = std::sqrt(0.125 / std::pow(10.0, snrDb / 10.0) / 0.75);
    Rng r(seed);
    for (auto& v : a) v += (float)(sigma * r.g());
    return a;
}
// 170 / 200 and 425 / 450 Hz are told apart by the spacing of two broad spectral lines, 10 to 30 Hz of it: the decoder works with either
bool shiftNear(int a, int b) { return std::fabs(kRttyShiftTable[a] - kRttyShiftTable[b]) <= 30; }
}   // namespace

int main(int argc, char** argv) {
    const bool sweep = argc > 1 && !strcmp(argv[1], "--sweep");
    const auto t0 = std::chrono::steady_clock::now();
    // 1. every speed x shift x polarity at 15 dB, the lower tone at various places
    int ok = 0, okNear = 0, n = 0, lowOk = 0;
    for (int b = 0; b < kRttyBauds; b++)
        for (int s = 0; s < kRttyShifts; s++)
            for (int rv = 0; rv < 2; rv++) {
                const double low = 400 + 45.0 * (double)((b * 5 + s) * 2 + rv);
                const auto a = rtty(b, s, rv != 0, low, 15, 14, (uint64_t)(100 + n));
                const auto h = rttyAutoAnalyse(a.data(), a.size());
                n++;
                const bool good = !h.empty() && h[0].baudIdx == b && h[0].shiftIdx == s && h[0].reverse == (rv != 0);
                const bool inList = [&] { for (const auto& x : h) if (x.baudIdx == b && x.shiftIdx == s && x.reverse == (rv != 0)) return true; return false; }();
                if (good) ok++;
                if (!h.empty() && h[0].baudIdx == b && h[0].reverse == (rv != 0) && shiftNear(h[0].shiftIdx, s)) okNear++;
                if (!h.empty() && std::fabs(h[0].lowHz - low) < 15) lowOk++;
                if (!good) printf("  baud %g shift %g rev %d low %.0f: got %s (in list: %d)\n", kRttyBaudTable[b], kRttyShiftTable[s], rv, low,
                                  h.empty() ? "nothing" : (std::string(std::to_string(h[0].baud)) + " / " + std::to_string(h[0].shiftHz) + (h[0].reverse ? " rev" : "")).c_str(), (int)inList);
            }
    printf("15 dB, all 40 combinations: %d of %d exactly right, %d right with the neighbouring shift (170/200, 425/450) counted, tone place %d\n", ok, n, okNear, lowOk);
    if (!sweep) CHECK(okNear == n && ok >= 36 && lowOk >= n - 1, "15 dB: %d exact, %d near, %d tones of %d", ok, okNear, lowOk, n);
    // 2. noise sweep on the common 45.45 baud / 170 Hz and a 75 baud / 425 Hz reversed signal
    for (double snr : {10.0, 5.0, 2.0, 0.0}) {
        int g = 0, tot = 0;
        for (int c = 0; c < 2; c++)
            for (int k = 0; k < 4; k++) {
                const int b = c ? 2 : 0, s = c ? 2 : 0;
                const auto a = rtty(b, s, c == 1, 1400 + 130.0 * k, snr, 16, (uint64_t)(300 + k + 10 * c));
                const auto h = rttyAutoAnalyse(a.data(), a.size());
                tot++;
                g += !h.empty() && h[0].baudIdx == b && shiftNear(h[0].shiftIdx, s) && h[0].reverse == (c == 1);
            }
        printf("%4.0f dB: %d of %d right\n", snr, g, tot);
        if (!sweep && snr >= 10) CHECK(g == tot, "%.0f dB: %d of %d", snr, g, tot);
    }
    // 3. nothing there: noise alone, and a steady tone
    {
        int bad = 0;
        for (int k = 0; k < 6; k++) {
            Rng r((uint64_t)(900 + k));
            std::vector<float> a(16 * 8000);
            for (size_t i = 0; i < a.size(); i++) a[i] = (float)(0.2 * r.g() + (k >= 3 ? 0.3 * std::sin(6.283185307179586 * 1500.0 * (double)i / 8000.0) : 0.0));
            bad += !rttyAutoAnalyse(a.data(), a.size()).empty();
        }
        printf("noise / steady tone: %d false locks of 6\n", bad);
        CHECK(bad == 0, "%d false locks", bad);
    }
    // 4. through the decoder: autoLock() with a wrong setting
    if (!sweep) {
        const auto a = rtty(2, 3, true, 1900, 12, 24, 77);   // 75 baud, 450 Hz, reversed
        auto dec = makeRttyDecoder();
        dec->setBaudIndex(0); dec->setShiftIndex(0);
        dec->reset();
        dec->autoLock();
        for (size_t i = 0; i < a.size(); i += 800) dec->feedAudio(&a[i], std::min<size_t>(800, a.size() - i));
        HfdigRttyTelemetry t;
        dec->telemetry(t);
        printf("autoLock: state %d, baud %g shift %g reverse %d, %d guesses, %llu chars\n", t.autoState, t.baud, t.shiftHz, (int)t.reverse, (int)t.autoHyps.size(), (unsigned long long)t.chars);
        CHECK(t.autoState == 2 && t.baud == 75.0 && std::fabs(t.shiftHz - 450.0) <= 30 && t.reverse, "autoLock result");
        CHECK(t.chars > 100, "autoLock: only %llu chars after locking", (unsigned long long)t.chars);
    }
    printf("%.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return testSummary("hfdig_rttyauto");
}
