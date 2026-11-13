// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Weak weather fax (issue #103): the picture of a weak signal comes through the narrow filter in front of the discriminator. With the
// start tone and the phasing at 6 dB and 2 dB (white noise over 0 .. 6 kHz, the fax occupies about a quarter of it), and joined in the
// middle of a picture (no start tone, no phasing) at 4 dB: the line lock holds and the rows still look like the chart. Before the narrow
// filter the same signals gave row correlations of 0.50, 0.27 and 0.65.
#include "../data/marine/fax/fax_util.h"
using namespace faxt;

// mean row correlation of the decoded picture against the chart, at the best row offset in [k0, k1]
static double bestCorr(const FaxImage& im, const FaxImage& ref, int k0, int k1, int& kBest) {
    double best = -9;
    kBest = 0;
    if (im.width != ref.width) return best;
    for (int k = k0; k <= k1; k++) {
        double s = 0;
        int n = 0;
        for (int r = 10; r < im.height - 10 && r + k < ref.height; r += 5) {
            if (r + k < 0) continue;
            double sh, c;
            rowShift(&im.pix[static_cast<size_t>(r) * im.width], &ref.pix[static_cast<size_t>(r + k) * im.width], im.width, 40, sh, c);
            if (c > -1.5) { s += c; n++; }
        }
        if (n && s / n > best) { best = s / n; kBest = k; }
    }
    return best;
}

int main() {
    for (double snr : {6.0, 2.0}) {   // a whole transmission
        Opts o; o.lines = 100; o.snrDb = snr;
        const auto a = makeAudio(o);
        FaxDecoder d;
        d.configure(o.rate);
        feedAll(d, a, 4096);
        FaxImage im; uint64_t seq = 0;
        d.latestImage(im, seq);
        const FaxStatus st = d.status();
        const Quality q = compare(im, faxTestChart(faxImageWidth(576), 100, 1), 1, 20);
        printf("whole transmission at %.0f dB: rows %d, lpm %d, drift %.2f px, row corr %.3f\n", snr, im.height, st.lpm, q.drift, q.meanCorr);
        FCHECK(im.height == 101 && st.lpm == 120 && std::fabs(q.drift) < 1.0, "%.0f dB: line lock (rows %d, drift %.2f)", snr, im.height, q.drift);
        FCHECK(q.meanCorr > (snr > 4 ? 0.64 : 0.40), "%.0f dB: row correlation %.3f", snr, q.meanCorr);
    }
    {   // joined 50 s in: the picture runs from 35 s, so the first decoded row is chart row 30 or so
        Opts o; o.lines = 200; o.snrDb = 4;
        const auto a = makeAudio(o);
        FaxDecoder d;
        d.configure(o.rate);
        feedAll(d, std::vector<float>(a.begin() + static_cast<size_t>(50 * o.rate), a.end()), 4096);
        FaxImage im; uint64_t seq = 0;
        d.latestImage(im, seq);
        int k = 0;
        const double c = bestCorr(im, faxTestChart(faxImageWidth(576), 200, 1), 20, 45, k);
        printf("joined mid-picture at 4 dB: rows %d, lpm %d, row corr %.3f at row offset %d\n", im.height, d.status().lpm, c, k);
        FCHECK(im.height > 150 && d.status().lpm == 120, "mid-picture: rows %d lpm %d", im.height, d.status().lpm);
        FCHECK(c > 0.72 && k >= 28 && k <= 34, "mid-picture: row correlation %.3f at offset %d", c, k);
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
