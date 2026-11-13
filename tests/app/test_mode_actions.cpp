// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The fine-frequency snap of "Auto setup" (app/ui/kit/mode_actions_logic.h): finds a narrow signal near the tuned frequency, leaves noise and wide captures alone.
#include "../../app/ui/kit/mode_actions_logic.h"
#include <cstdio>
#include <cstdlib>
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
using namespace modeactions;

// a DC-centred frame with noise at -90 dBFS and a 3-bin signal at sigHz
static std::vector<float> frame(size_t n, double fs, double sigHz, float level) {
    std::vector<float> d(n);
    srand(7);
    for (auto& x : d) x = -90.f + (float)(rand() % 100) / 50.f;
    if (level > -200) { const long c = (long)(sigHz / (fs / (double)n) + (double)n / 2); for (long k = -1; k <= 1; k++) d[(size_t)(c + k)] = level - (k ? 3.f : 0.f); }
    return d;
}

int main() {
    const double fs = 1e6; const size_t n = 8192;   // 122 Hz bins
    double out = 0;
    { auto d = frame(n, fs, 7000, -40); CHECK(refineOffsetHz(d, fs, 0, 20e3, &out) && std::fabs(out - 7000) < 250, "finds a signal 7 kHz off: %.0f", out); }
    { auto d = frame(n, fs, -12000, -40); CHECK(refineOffsetHz(d, fs, -10000, 20e3, &out) && std::fabs(out + 12000) < 250, "tuned off-centre, signal 2 kHz lower: %.0f", out); }
    { auto d = frame(n, fs, 0, -300); CHECK(!refineOffsetHz(d, fs, 0, 20e3, &out), "noise only: nothing found"); }
    { auto d = frame(n, fs, 40000, -40); CHECK(!refineOffsetHz(d, fs, 0, 20e3, &out), "a signal outside the window is not chased"); }
    { auto d = frame(n, fs, 100, -40); CHECK(refineOffsetHz(d, fs, 0, 5e3, &out) && std::fabs(out - 100) < 250, "already near: stays near"); }
    { std::vector<float> d(8); CHECK(!refineOffsetHz(d, fs, 0, 20e3, &out), "tiny frame"); }
    { auto d = frame(n, fs, 0, -40); CHECK(!refineOffsetHz(d, fs, 0, 100, &out), "window of under 3 bins"); }
    CHECK(snapWindowHz(0.25) == 25e3 || snapWindowHz(0.25) <= 25e3, "narrow window capped");
    CHECK(snapWindowHz(8.0) == 0 && snapWindowHz(2.4) == 0, "a wide capture gets no snap");
    CHECK(snapWindowHz(0.012) >= 2e3, "a very narrow capture still gets a usable window");
    CHECK(snapWindowHz(0) == 0, "no bandwidth, no snap");
    std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
