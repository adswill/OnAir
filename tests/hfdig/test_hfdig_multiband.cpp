// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Multi-band monitor, the logic (hfdig_multiband.h): dial frequencies, coverage of a capture, the best centre, the needed rate, the
// preference mask and the CPU guard. No DSP here; the decoding is test_hfdig_multiband_dsp.
#include "dect2/hfdig/hfdig_multiband.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    // dial frequencies of the table are those of hf.tsv
    CHECK(mbDialHz(5, 0) == 14074000 && mbDialHz(5, 1) == 14080000 && mbDialHz(5, 2) == 14084000 && mbDialHz(5, 3) == 14095600, "20 m dials");
    CHECK(mbDialHz(0, 1) == 0 && mbDialHz(0, 2) == 0 && mbDialHz(4, 2) == 0, "no FT4 on 160 m, FT2 only on 20 m");
    // the Band column of a decode: the dial it carries maps back to the band it came from, for every band with an FT8 dial (5 or more bands)
    { int n = 0; for (int b = 0; b < kMbBands; b++) { const double f = mbDialHz(b, 0); if (f > 0) { n++; CHECK(mbBandOf(f) == b, "FT8 dial of band %d (%.4f MHz) maps to its band, got %d", b, f / 1e6, mbBandOf(f)); } } CHECK(n >= 5, "at least 5 bands have an FT8 dial: %d", n); }
    CHECK(std::string(mbBand(0).name) == "160 m" && std::string(mbBand(10).name) == "6 m", "band names");
    for (int b = 0; b < kMbBands; b++)
        for (int m = 0; m < kMbModes; m++) {
            const double f = mbDialHz(b, m);
            CHECK(f == 0 || (f >= mbBand(b).loHz - 3000 && f <= mbBand(b).hiHz), "dial %d/%d lies in its band (%.4f MHz)", b, m, f / 1e6);
        }

    // RTL-SDR V4, 2.4 MHz, tuned to 14 MHz: the four 20 m modes are in, 17 m and 40 m are not
    const std::vector<MbSel> twenty = {{5, 0}, {5, 1}, {5, 2}, {5, 3}};
    for (const MbCover& c : mbCoverage(14000000, 2400000, twenty)) CHECK(c.covered, "RTL at 14.000: %.4f MHz covered", c.dialHz / 1e6);
    std::vector<MbSel> mix = {{5, 0}, {3, 0}, {6, 0}, {0, 1}};
    auto cv = mbCoverage(14000000, 2400000, mix);
    CHECK(cv[0].covered && !cv[1].covered && !cv[2].covered, "40 m and 17 m are outside 2.4 MHz around 14.000");
    CHECK(!cv[3].covered && cv[3].dialHz == 0 && cv[3].neededSpanHz == 0, "160 m FT4 has no dial frequency: not covered, nothing to retune for");
    const double need = cv[1].neededSpanHz;
    CHECK(need > 2 * (14074000 - 7074000) && need < 2.6 * (14074000 - 7074000), "span needed for 40 m from 14.000 is %.1f MHz", need / 1e6);
    // the edge: usable is 80 % of the rate, minus the 3.3 kHz of audio
    CHECK(mbCoverage(14074000 - 950000, 2400000, {{5, 0}})[0].covered == true, "inside the 80 %%: covered");
    CHECK(mbCoverage(14074000 - 970000, 2400000, {{5, 0}})[0].covered == false, "past the 80 %%: not covered");
    CHECK(!mbCoverage(14074000, 0, {{5, 0}})[0].covered, "no rate: nothing covered");

    // wide radio: 10 Msps (an RX888 at 64 Msps would hold even more) holds 40 m .. 20 m together
    std::vector<MbSel> hf = {{3, 0}, {5, 0}, {4, 3}};
    int n = 0;
    double c = mbBestCentre(10000000, hf, &n);
    CHECK(n == 3 && c > 7.0e6 && c < 14.2e6, "10 Msps holds 40 / 30 / 20 m: %d at %.3f MHz", n, c / 1e6);
    for (const MbCover& k : mbCoverage(c, 10000000, hf)) CHECK(k.covered, "best centre covers %.4f MHz", k.dialHz / 1e6);
    c = mbBestCentre(2400000, {{3, 0}, {5, 0}, {5, 1}, {6, 0}}, &n);
    CHECK(n == 2 && std::fabs(c - 14077000) < 1, "2.4 MHz: the most is 20 m FT8 + FT4 (%d at %.4f MHz)", n, c / 1e6);
    c = mbBestCentre(2400000, {{3, 0}, {3, 1}, {5, 0}}, &n);
    CHECK(n == 2 && c < 7.2e6, "two on 40 m beat one on 20 m (%d at %.4f MHz)", n, c / 1e6);
    mbBestCentre(2400000, {}, &n);
    CHECK(n == 0, "nothing selected: 0 covered");
    const double r = mbNeededRate({{3, 0}, {6, 0}});
    CHECK(r > 13.5e6 && r < 14.0e6, "7.074 .. 18.100 MHz needs %.2f Msps", r / 1e6);
    CHECK(mbNeededRate({{5, 0}}) < 20000, "one dial needs only a few kHz");

    // the preference mask keeps the selection
    const std::vector<MbSel> all = {{0, 0}, {5, 2}, {10, 3}, {7, 1}};
    const uint64_t m = mbMask(all);
    const auto back = mbFromMask(m);
    CHECK(back.size() == 4, "mask round trip: %zu", back.size());
    for (const MbSel& s : all) { bool f = false; for (const MbSel& t : back) f |= (s == t); CHECK(f, "mask keeps %d/%d", s.band, s.mode); }
    CHECK(mbMask({{99, 0}, {0, 9}}) == 0, "out of range selections are dropped");

    // CPU guard: fine at 50 %, acts after 8 blocks over 85 %, comes back, no action when the load falls again
    {
        MbCpuGuard g;
        bool act = false;
        for (int i = 0; i < 40; i++) act |= g.update(0.5, 1.0);
        CHECK(!act && !g.tooSlow() && g.load() > 0.45 && g.load() < 0.55, "50 %% load: no action (%.2f)", g.load());
        int when = -1;
        for (int i = 0; i < 40 && when < 0; i++) if (g.update(2.0, 1.0)) when = i;
        CHECK(when >= 7 && when < 30 && g.tooSlow(), "overload acts after a few blocks (block %d)", when);
        MbCpuGuard h;
        for (int i = 0; i < 6; i++) h.update(1.5, 1.0);
        for (int i = 0; i < 30; i++) h.update(0.2, 1.0);
        CHECK(!h.tooSlow(), "load falls: the hint goes away");
    }
    // the receiver drops the least useful band when the guard asks; one band is never dropped
    {
        auto main = makeFtxDecoder();
        HfdigMultiBand mb(*main);
        mb.configure(10000000, 16000000, {{3, 0}, {5, 0}, {6, 0}});      // 40, 20, 17 m inside 10 Msps? 7.074 is 8.9 MHz away: only 20 / 17 m
        MbStatus s = mb.status();
        CHECK(s.bands == 2 && s.active.size() == 2 && s.notCovered.size() == 1, "10 Msps at 16 MHz: 20 + 17 m in, 40 m out (%d bands)", s.bands);
        for (int i = 0; i < 20; i++) mb.testForceLoad(3.0, 1.0);
        s = mb.status();
        CHECK(s.bands == 1 && s.dropped == 1 && s.tooSlow, "too slow: one band dropped (%d left, %d dropped)", s.bands, s.dropped);
        for (int i = 0; i < 40; i++) mb.testForceLoad(3.0, 1.0);
        s = mb.status();
        CHECK(s.bands == 1, "the last band stays (%d)", s.bands);
    }
    return testSummary("hfdig_multiband");
}
