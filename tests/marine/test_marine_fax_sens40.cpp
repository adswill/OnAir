// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 4.0 sensitivity (docs/SENSITIVITY-4.0-X2.md): the weather fax row filter (weak signals: a local Wiener filter along the row with the noise
// measured on the row) against the same decoder with it off (3.9), same noise. Conditions of test_marine_fax_sens39.cpp: the test chart,
// 100 lines at 120 lpm, white noise over 0 .. 6 kHz, +-30 Hz tuning error, +-20 ppm clock. Prints the mean row correlation and the sens39
// pass count (line lock and mean correlation over 0.5) per SNR; guard: at 2 and 0 dB the filter raises the mean correlation by 0.02 or more,
// at 10 dB it loses less than 0.01, and the line lock is the same both ways. Simulated signals only.
#include "../data/marine/fax/fax_util.h"
#include <cstdlib>
using namespace faxt;

namespace {

struct R { double corr = 0; bool pass = false; int rows = 0; };

R trial(double snr, uint32_t seed, bool filter) {
    Opts o; o.lines = 100; o.snrDb = snr; o.seed = seed; o.mistuneHz = ((seed & 1) ? 1 : -1) * 30.0; o.ppm = ((seed & 2) ? 1 : -1) * 20.0;
    const auto a = makeAudio(o);
    FaxDecoder d;
    d.configure(o.rate);
    d.setNoiseFilter(filter);
    feedAll(d, a, 4096);
    FaxImage im; uint64_t seq = 0;
    d.latestImage(im, seq);
    const FaxStatus st = d.status();
    const Quality q = compare(im, faxTestChart(faxImageWidth(576), 100, seed), 1, 20);
    R r; r.corr = q.meanCorr; r.rows = im.height;
    r.pass = im.height == 101 && st.lpm == 120 && std::fabs(q.drift) < 1.0 && q.meanCorr > 0.5;
    return r;
}

} // namespace

int main() {
    double g2 = 0, g0 = 0, g10 = 0;
    for (double snr : {10.0, 4.0, 2.0, 1.0, 0.0, -1.0, -2.0}) {
        double c0 = 0, c1 = 0; int p0 = 0, p1 = 0, lockDiff = 0;
        for (uint32_t i = 0; i < 3; i++) {
            const uint32_t seed = 300 + (uint32_t)(snr * 10 + 100) * 7 + i;
            const R a = trial(snr, seed, false), b = trial(snr, seed, true);
            c0 += a.corr / 3; c1 += b.corr / 3; p0 += a.pass; p1 += b.pass; lockDiff += a.rows != b.rows;
        }
        printf("fax %4.1f dB: mean row correlation %.3f -> %.3f (%+.3f), passes %d -> %d of 3\n", snr, c0, c1, c1 - c0, p0, p1);
        fflush(stdout);
        FCHECK(lockDiff == 0, "%.0f dB: rows differ with the filter", snr);
        if (snr == 2.0) g2 = c1 - c0;
        if (snr == 0.0) g0 = c1 - c0;
        if (snr == 10.0) g10 = c1 - c0;
    }
    FCHECK(g2 >= 0.02 && g0 >= 0.02, "gain %.3f at 2 dB, %.3f at 0 dB", g2, g0);
    FCHECK(g10 > -0.01, "10 dB: change %.3f", g10);
    printf(fails ? "marine_fax_sens40: %d FAILED\n" : "marine_fax_sens40: all passed\n", fails);
    return fails ? 1 : 0;
}
