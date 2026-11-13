// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FM audio tone control: the shelves' gains read from their coefficients (DC, the corner, high frequencies, the Warm cut), the
// stability of every setting, the treble and Warm mapping, and the processing (a DC level through a bass boost, the off state passes the audio untouched).
#include "dect2/fm/fm_tone.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const double fs = 48000;
    // the identity: no shelf, no Warm, nothing to do
    {
        FmTone off; off.configure(0, 0, false, fs);
        CHECK(!off.active(), "all zero is off");
        std::vector<float> l(64), r(64);
        for (size_t i = 0; i < l.size(); i++) { l[i] = 0.1f * (float)i; r[i] = -l[i]; }
        const std::vector<float> l0 = l, r0 = r;
        off.process(l.data(), r.data(), l.size());
        CHECK(l == l0 && r == r0, "off passes the audio untouched");
        FmTone warm; warm.configure(0, 0, true, fs);
        CHECK(warm.active(), "warm alone is on");
        CHECK(std::fabs(fmShelfDb(fmLowShelf(0, kToneBassHz, fs), 50, fs)) < 1e-9, "a 0 dB low shelf is flat");
    }
    // bass: +6 dB at DC, nothing up the top; -6 dB the other way; the corner sits at half the gain
    {
        const FmShelf up = fmLowShelf(6, kToneBassHz, fs), down = fmLowShelf(-6, kToneBassHz, fs);
        CHECK(std::fabs(fmShelfDb(up, 1, fs) - 6) < 0.05, "bass +6 at DC: %.3f dB", fmShelfDb(up, 1, fs));
        CHECK(std::fabs(fmShelfDb(down, 1, fs) + 6) < 0.05, "bass -6 at DC: %.3f dB", fmShelfDb(down, 1, fs));
        CHECK(std::fabs(fmShelfDb(up, 5000, fs)) < 0.2, "bass +6 at 5 kHz: %.3f dB", fmShelfDb(up, 5000, fs));
        CHECK(std::fabs(fmShelfDb(up, kToneBassHz, fs) - 3) < 0.05, "bass +6 at the corner: %.3f dB", fmShelfDb(up, kToneBassHz, fs));
    }
    // treble: +6 dB up the top, nothing at 200 Hz; the corner at half the gain
    {
        const FmShelf up = fmHighShelf(6, kToneTrebleHz, fs), down = fmHighShelf(-6, kToneTrebleHz, fs);
        CHECK(std::fabs(fmShelfDb(up, 20000, fs) - 6) < 0.3, "treble +6 at 20 kHz: %.3f dB", fmShelfDb(up, 20000, fs));
        CHECK(std::fabs(fmShelfDb(down, 20000, fs) + 6) < 0.3, "treble -6 at 20 kHz: %.3f dB", fmShelfDb(down, 20000, fs));
        CHECK(std::fabs(fmShelfDb(up, 200, fs)) < 0.1, "treble +6 at 200 Hz: %.3f dB", fmShelfDb(up, 200, fs));
        CHECK(std::fabs(fmShelfDb(up, kToneTrebleHz, fs) - 3) < 0.05, "treble +6 at the corner: %.3f dB", fmShelfDb(up, kToneTrebleHz, fs));
    }
    // Warm: a gentle cut above 6 kHz only, -3 dB well above the corner
    {
        const FmShelf warm = fmHighShelf(fmTrebleGainDb(0, true), kToneTrebleHz, fs);
        CHECK(std::fabs(fmShelfDb(warm, 1000, fs)) < 0.1, "warm at 1 kHz: %.3f dB", fmShelfDb(warm, 1000, fs));
        CHECK(std::fabs(fmShelfDb(warm, kToneTrebleHz, fs) + 1.5) < 0.05, "warm at 6 kHz: %.3f dB", fmShelfDb(warm, kToneTrebleHz, fs));
        CHECK(fmShelfDb(warm, 12000, fs) < -2.3 && fmShelfDb(warm, 12000, fs) > -3.1, "warm at 12 kHz: %.3f dB", fmShelfDb(warm, 12000, fs));
        CHECK(fmTrebleGainDb(0, true) == -kWarmCutDb && fmTrebleGainDb(6, true) == 3 && fmTrebleGainDb(-6, false) == -6, "warm adds to the treble control");
        CHECK(fmTrebleGainDb(20, false) == 6 && fmTrebleGainDb(-20, true) == -9, "the control clamps to +-6 dB");
    }
    // every setting is stable: the poles of 1 + a1 z^-1 + a2 z^-2 lie inside the unit circle (Jury test)
    {
        int bad = 0;
        for (int db = -6; db <= 6; db++) for (int w = 0; w < 2; w++) {
            const FmShelf s = w ? fmHighShelf(fmTrebleGainDb(db, true), kToneTrebleHz, fs) : fmLowShelf(db, kToneBassHz, fs);
            if (!(std::fabs(s.a2) < 1 && std::fabs(s.a1) < 1 + s.a2)) bad++;
        }
        CHECK(bad == 0, "%d unstable settings", bad);
    }
    // the processing: a DC level of 1 through bass +6 settles at 10^(6/20), the left and right channels apart
    {
        FmTone tone; tone.configure(6, 0, false, fs);
        std::vector<float> l(24000, 1.f), r(24000, -1.f);
        tone.process(l.data(), r.data(), l.size());
        const double g = std::pow(10.0, 6.0 / 20);
        CHECK(std::fabs(l.back() - g) < 0.02 && std::fabs(r.back() + g) < 0.02, "bass +6 DC: %.4f %.4f", l.back(), r.back());
        tone.reset();
        std::vector<float> z(16, 0.f), zr(16, 0.f);
        tone.process(z.data(), zr.data(), z.size());
        CHECK(z[15] == 0.f && zr[15] == 0.f, "silence stays silent after reset");
    }
    return testSummary("fm_tone");
}
