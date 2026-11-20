// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Digital video generator, tab "Digital Television Mode Selector" (dect2/tv/dvb_modes.h), without a window:
//  1. the number of modes per system: those of DVB_transmission_modes.xlsx, the satellite ones once per roll-off
//  2. bandwidth, bitrate and C/N of modes of each system against the values the workbook computes (all 34 980 rows were compared
//     once when the tables were written; these keep them honest)
//  3. the mode names
//  4. the link fit: the bitrate, the bandwidth and the C/N each rule a mode out; satellite modes scale with the symbol rate
//  5. the available modes, the best first, and the filters
//  6. DVB-T2-Lite (EN 302 755 annex I): which modes exist, two bitrates by hand, the 4 Mbit/s cap, the Profile filter, old saved filters
#include "dect2/tv/dvb_modes.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>

using namespace dect2::vgen;

static int failures = 0;
static void fail(const char* fmt, ...) {   // one printf with a constant format, as Codacy wants
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    std::printf("FAIL: %s\n", b);
    failures++;
}
#define CHECK(c, ...) do { if (!(c)) fail(__VA_ARGS__); } while (0)

static bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol * std::max(1.0, std::fabs(b)); }

static const DvbMode* find(DvbSystem s, const std::string& name) {
    for (const DvbMode& m : dvbModes(s))
        if (modeName(m) == name) return &m;
    return nullptr;
}

// a mode by name, with the workbook's bandwidth (MHz), bitrate (Mbit/s) and C/N (dB); satellite at 1 Msym/s
static void expectMode(DvbSystem s, const std::string& name, double bw, double rate, double cn) {
    const DvbMode* m = find(s, name);
    if (!m) { fail("no mode %s", name.c_str()); return; }
    CHECK(near(m->bwMHz, bw), "%s: bandwidth %.9f, workbook %.9f", name.c_str(), m->bwMHz, bw);
    CHECK(near(m->rateMbps, rate), "%s: bitrate %.9f, workbook %.9f", name.c_str(), m->rateMbps, rate);
    CHECK(m->hasCn && near(m->cnDb, cn), "%s: C/N %.6f, workbook %.6f", name.c_str(), m->cnDb, cn);
}

static void testCounts() {
    // DVB-T2: 28 560 T2-Base modes as before, plus T2-Lite. T2-Lite per channel width: FFT / guard / pilot sets of table I.5 that have a
    // Cdata entry: 2K 7, 4K 7, 8K 12 and 16K 13, the last two in normal and extended carrier mode: 7 + 7 + 24 + 26 = 64; times the
    // constellation / code rate pairs of table I.4: 6 + 6 + 6 + 4 (256-QAM without 2/3 and 3/4) = 22; one FEC frame (16 200): 1 408 a width.
    // DVB-T2 has five widths: 28 560 + 7 040 = 35 600; the 1,7 MHz system one: 5 712 + 1 408 = 7 120.
    const size_t want[] = {480, 35600, 7120, 5, 312, 714};
    for (int s = 0; s < kNumDvbSystems; s++) {
        const size_t n = dvbModes((DvbSystem)s).size();
        CHECK(n == want[s], "%s: %zu modes, expected %zu", dvbSystemName((DvbSystem)s), n, want[s]);
        CHECK(!modeParams((DvbSystem)s).empty(), "%s: no parameters", dvbSystemName((DvbSystem)s));
    }
    size_t noCn = 0;
    for (const DvbMode& m : dvbModes(DvbSystem::S2)) noCn += m.hasCn ? 0 : 1;
    CHECK(noCn == 24 * 2 * 3, "DVB-S2: %zu modes without a C/N, expected the 24 short MODCODs x pilots x roll-off", noCn);
}

static void testValues() {
    expectMode(DvbSystem::T, "T-8MHz-2K-GI1/4-QPSK-1/2", 7.6116071428571432, 4.9764705882352942, 3.5);
    expectMode(DvbSystem::T, "T-8MHz-8K-GI1/4-64-QAM-2/3", 7.608258929, 19.905882353, 16.7);
    expectMode(DvbSystem::T2, "T2-5MHz-1K-GI1/16-PP4-QPSK-1/2-N", 4.7600446428571432, 4.1872393401805166, 1.42);
    expectMode(DvbSystem::T2Narrow, "T2-1.7MHz-1K-GI1/16-PP4-QPSK-1/2-N", 1.536958076584507, 1.3520065052625116, 1.42);
    expectMode(DvbSystem::T2, "T2-8MHz-32KE-GI1/128-PP7-256-QAM-3/5-N", 7.768136161, 36.162504683, 16.43);
    expectMode(DvbSystem::S, "S-QPSK-1/2", 1.35, 0.92156862745098034, 4.145276818377809);
    expectMode(DvbSystem::S2, "S2-QPSK-1/4-N-POff-RO0.20", 1.2, 0.49024315173899663, -2.35);
    expectMode(DvbSystem::S2X, "S2X-QPSK 13/45-N-POff-RO0.20", 1.2, 0.56780547860880271, -2.03);
    expectMode(DvbSystem::S2X, "S2X-BPSK-S 1/5-VL1-RO0.05", 1.05, 0.073072532, -9.9);
    const DvbMode* shortFrame = find(DvbSystem::S2, "S2-16APSK-3/4-S-POn-RO0.35");
    CHECK(shortFrame && !shortFrame->hasCn && near(shortFrame->rateMbps, 2.761633428), "DVB-S2 short frame: bitrate, and no C/N from Table 13");
}

static void testFit() {
    const DvbMode* t = find(DvbSystem::T, "T-8MHz-8K-GI1/4-64-QAM-2/3");   // 7.61 MHz, 19.9 Mbit/s, 16.7 dB
    if (!t) { fail("no DVB-T 64-QAM 2/3 mode"); return; }
    ModeFit f = fitMode(*t, {19000, 8, 20});
    CHECK(f.ok && near(f.marginDb, 3.3) && near(f.bwMHz, t->bwMHz) && f.symbolRate == 0, "DVB-T: 19 Mbit/s in 8 MHz at 20 dB fits with 3.3 dB margin");
    CHECK(!fitMode(*t, {20000, 8, 20}).ok, "DVB-T: 20 Mbit/s is more than the mode carries");
    CHECK(!fitMode(*t, {19000, 7.5, 20}).ok, "DVB-T: 7.5 MHz is less than the 7.61 MHz the mode occupies");
    CHECK(!fitMode(*t, {19000, 8, 16.6}).ok, "DVB-T: 16.6 dB is below the 16.7 dB the mode needs");
    CHECK(fitMode(*t, {19000, 8, 16.7}).ok, "DVB-T: exactly the minimum C/N is enough");

    const DvbMode* s = find(DvbSystem::S2, "S2-QPSK-1/4-N-POff-RO0.20");   // 0.490 bit/symbol, 1.2 MHz per Msym/s
    if (!s) { fail("no DVB-S2 QPSK 1/4 mode"); return; }
    f = fitMode(*s, {1000, 3, 0});
    CHECK(f.ok && near(f.symbolRate, 1 / s->rateMbps) && near(f.bwMHz, 1.2 / s->rateMbps), "DVB-S2: 1 Mbit/s needs %.4f Msym/s in %.4f MHz", f.symbolRate, f.bwMHz);
    CHECK(near(f.capacityMbps, 3 / 1.2 * s->rateMbps), "DVB-S2: capacity in 3 MHz %.4f Mbit/s", f.capacityMbps);
    CHECK(!fitMode(*s, {1000, 2.4, 0}).ok, "DVB-S2: 1 Mbit/s at QPSK 1/4 needs 2.45 MHz, more than 2.4");
    CHECK(fitMode(*s, {1000, f.bwMHz, 0}).ok, "DVB-S2: exactly the bandwidth needed is enough");
    CHECK(!fitMode(*s, {1000, 3, -2.4}).ok, "DVB-S2: -2.4 dB is below the -2.35 dB QPSK 1/4 needs");

    const DvbMode* sh = find(DvbSystem::S2, "S2-QPSK-1/4-S-POff-RO0.20");
    CHECK(sh && !fitMode(*sh, {100, 10, 30}).ok, "DVB-S2: a short-frame mode without a C/N threshold never counts as available");
}

static void testAvailable() {
    // 19 Mbit/s in 8 MHz at 20 dB: the smallest channel that carries it is 6 MHz (5 MHz would need 64-QAM 7/8, 20.2 dB); of its modes
    // 64-QAM 3/4 with GI 1/32 has the most margin, and 8K occupies a little less than 2K
    std::vector<ModeChoice> t = availableModes(DvbSystem::T, openFilter(), {19000, 8, 20});
    CHECK(!t.empty() && modeName(*t.front().mode) == "T-6MHz-8K-GI1/32-64-QAM-3/4", "DVB-T best: %s", t.empty() ? "(none)" : modeName(*t.front().mode).c_str());
    for (size_t i = 1; i < t.size(); i++)
        if (betterChoice(t[i], t[i - 1])) { fail("DVB-T: the list is not sorted best first at %zu", i); break; }
    for (const ModeChoice& c : t) CHECK(c.fit.ok && c.fit.capacityMbps >= 19 && c.fit.marginDb >= 0, "DVB-T: %s listed but does not fit", modeName(*c.mode).c_str());

    // a filter: QPSK alone carries at most 8.3 Mbit/s
    ModeFilter qpsk = openFilter();
    qpsk[3] = 0;
    CHECK(availableModes(DvbSystem::T, qpsk, {19000, 8, 20}).empty(), "DVB-T QPSK: nothing carries 19 Mbit/s");
    CHECK(!availableModes(DvbSystem::T, qpsk, {5000, 8, 20}).empty(), "DVB-T QPSK: 5 Mbit/s fits");
    for (const ModeChoice& c : availableModes(DvbSystem::T, qpsk, {5000, 8, 20}))
        CHECK(c.mode->value[3] == 0, "DVB-T QPSK filter let %s through", modeName(*c.mode).c_str());
    CHECK(availableModes(DvbSystem::T, openFilter(), {1000, 8, 3}).empty(), "DVB-T: nothing works below 3.5 dB");

    // DVB-S2, 2 Mbit/s in 3 MHz at 6 dB: the highest efficiency within 6 dB is 8PSK 3/5 (5.5 dB), at the smallest roll-off, no pilots
    std::vector<ModeChoice> s2 = availableModes(DvbSystem::S2, openFilter(), {2000, 3, 6});
    CHECK(!s2.empty() && modeName(*s2.front().mode) == "S2-8PSK-3/5-N-POff-RO0.20", "DVB-S2 best: %s", s2.empty() ? "(none)" : modeName(*s2.front().mode).c_str());
    // the 1,7 MHz DVB-T2 channel is the only terrestrial one within 2 MHz, and it is its own system
    CHECK(availableModes(DvbSystem::T2, openFilter(), {1000, 2, 30}).empty(), "DVB-T2: a 5-10 MHz mode fits in 2 MHz");
    const std::vector<ModeChoice> nb = availableModes(DvbSystem::T2Narrow, openFilter(), {1000, 2, 30});
    CHECK(!nb.empty() && nb.front().fit.bwMHz < 1.6, "DVB-T2 1.7 MHz: no mode for 1 Mbit/s in 2 MHz");
    for (const DvbMode& m : dvbModes(DvbSystem::T2))
        if (modeName(m).starts_with("T2-1.7MHz")) { fail("DVB-T2 has the 1,7 MHz mode %s", modeName(m).c_str()); break; }
}

static void testLite() {
    // every T2-Lite mode obeys annex I; every T2-Base mode is as before (no 1/3 or 2/5)
    size_t lite = 0, base = 0;
    for (DvbSystem sys : {DvbSystem::T2, DvbSystem::T2Narrow})
        for (const DvbMode& m : dvbModes(sys)) {
            const int fft = m.value[1], pp = m.value[4], mod = m.value[5], cr = m.value[6], fr = m.value[7];
            if (m.value[kT2ProfileParam] == 0) {
                base++;
                CHECK(cr < 6 && m.capMbps == 0, "T2-Base mode %s has a T2-Lite code rate or a cap", modeName(m).c_str());
                continue;
            }
            lite++;
            const std::string n = modeName(m);
            CHECK(n.ends_with("-Lite"), "%s: a T2-Lite name ends in -Lite", n.c_str());
            CHECK(fft >= 1 && fft <= 4, "%s: FFT 1K or 32K (I.7.1)", n.c_str());
            CHECK(pp != 7, "%s: PP8 (I.7.2)", n.c_str());
            CHECK(fr == 1, "%s: 64 800-bit frames (I.3)", n.c_str());
            CHECK(cr != 4 && cr != 5, "%s: code rate 4/5 or 5/6 (table I.1)", n.c_str());
            CHECK(!(mod == 3 && (cr == 2 || cr == 3)), "%s: 256-QAM 2/3 or 3/4 (table I.4)", n.c_str());
            CHECK(near(m.capMbps, 4.0), "%s: no 4 Mbit/s cap (I.10)", n.c_str());
            CHECK(m.hasCn == (cr < 6), "%s: a C/N where the standards give one, none for 1/3 and 2/5", n.c_str());
        }
    CHECK(lite == 7040 + 1408 && base == 28560 + 5712, "%zu T2-Lite and %zu T2-Base modes", lite, base);
    // two of the three combinations table I.5 drops against table 58 (16K 1/32 PP4, 16K 1/16 PP2) exist in T2-Base only
    CHECK(find(DvbSystem::T2, "T2-8MHz-16K-GI1/32-PP4-QPSK-1/2-S") && !find(DvbSystem::T2, "T2-8MHz-16K-GI1/32-PP4-QPSK-1/2-S-Lite"), "16K 1/32 PP4: Base only");
    CHECK(find(DvbSystem::T2, "T2-8MHz-16K-GI1/16-PP2-QPSK-1/2-S") && !find(DvbSystem::T2, "T2-8MHz-16K-GI1/16-PP2-QPSK-1/2-S-Lite"), "16K 1/16 PP2: Base only");

    // By hand, 1: 1,7 MHz, 8K, GI 1/4, PP1, QPSK 1/3. T = 71/131 µs (table 65), Tu = 8192 T = 4439.939 µs, Ts = 1.25 Tu = 5549.924 µs;
    // Cdata 6208 (table 48, 8K PP1); QPSK 2 bits; (Kbch - 80) / Nldpc = (5232 - 80) / 16200 (table I.1) = 0.318025;
    // 6208 x 2 x 0.318025 / 5549.924 µs = 0.711468 Mbit/s, under the cap. Bandwidth 6817 / Tu = 1.535381 MHz.
    const DvbMode* a = find(DvbSystem::T2Narrow, "T2-1.7MHz-8K-GI1/4-PP1-QPSK-1/3-S-Lite");
    CHECK(a && near(a->rateMbps, 0.7114682663884542) && near(a->bwMHz, 1.5353814783230633) && !a->hasCn, "1,7 MHz QPSK 1/3 by hand: %.9f Mbit/s",
          a ? a->rateMbps : 0.0);
    if (a) CHECK(near(fitMode(*a, {700, 2, 99}).capacityMbps, 0.7114682663884542) && !fitMode(*a, {700, 2, 99}).ok, "1/3: carries it, but no C/N, so never available");
    // By hand, 2: 8 MHz, 16K, GI 1/32, PP7, 64-QAM 2/5. T = 7/64 µs, Tu = 16384 T = 1792 µs, Ts = 33/32 Tu = 1848 µs; Cdata 13416 (16K PP7);
    // 6 bits; (6312 - 80) / 16200; 13416 x 6 x 0.384691 / 1848 µs = 16.756556 Mbit/s of cells, of which T2-Lite may use 4 (I.10).
    const DvbMode* b = find(DvbSystem::T2, "T2-8MHz-16K-GI1/32-PP7-64-QAM-2/5-S-Lite");
    CHECK(b && near(b->rateMbps, 16.756556036556038) && near(b->bwMHz, 7.607700892857143), "8 MHz 64-QAM 2/5 by hand: %.9f Mbit/s", b ? b->rateMbps : 0.0);
    if (b) CHECK(near(fitMode(*b, {3000, 8, 99}).capacityMbps, 4.0), "the T2-Lite cap: %.3f Mbit/s", fitMode(*b, {3000, 8, 99}).capacityMbps);

    // the cap: 5 Mbit/s is never T2-Lite; 3 Mbit/s at 20 dB is, with Profile T2-Lite or Any, never with T2-Base
    ModeFilter f = openFilter();
    f[kT2ProfileParam] = 1;
    for (const ModeChoice& c : availableModes(DvbSystem::T2, f, {5000, 8, 30})) { fail("T2-Lite carries 5 Mbit/s: %s", modeName(*c.mode).c_str()); break; }
    const auto lite3 = availableModes(DvbSystem::T2, f, {3000, 8, 20});
    CHECK(!lite3.empty(), "T2-Lite: 3 Mbit/s in 8 MHz at 20 dB");
    for (const ModeChoice& c : lite3) if (c.mode->value[kT2ProfileParam] != 1 || c.fit.capacityMbps > 4 + 1e-9) { fail("Profile T2-Lite listed %s", modeName(*c.mode).c_str()); break; }
    f[kT2ProfileParam] = 0;
    for (const ModeChoice& c : availableModes(DvbSystem::T2, f, {3000, 8, 20})) if (c.mode->value[kT2ProfileParam] != 0) { fail("Profile T2-Base listed %s", modeName(*c.mode).c_str()); break; }
    bool anyLite = false;
    for (const ModeChoice& c : availableModes(DvbSystem::T2, openFilter(), {3000, 8, 20})) anyLite |= c.mode->value[kT2ProfileParam] == 1;
    CHECK(anyLite, "Profile Any lists T2-Lite modes too");
    f[kT2ProfileParam] = -1;
    f[kT2CodeRateParam] = 6;   // 1/3: only T2-Lite modes, none of which has a C/N
    size_t n13 = 0;
    for (const DvbMode& m : dvbModes(DvbSystem::T2)) if (matches(m, f)) { n13++; CHECK(m.value[kT2ProfileParam] == 1, "1/3 in T2-Base"); }
    CHECK(n13 > 0 && availableModes(DvbSystem::T2, f, {1000, 8, 40}).empty(), "1/3: %zu modes, none proposed (no C/N)", n13);

    // saved filters: an old file (8 values, no Profile) loads with Profile on Any; the new text round-trips
    ModeFilter old = openFilter();
    filterFromText(DvbSystem::T2, "4,3,0,6,1,0,0,1", old);
    CHECK(old[0] == 4 && old[1] == 3 && old[3] == 6 && old[6] == 0 && old[7] == 1 && old[kT2ProfileParam] == -1, "an old DVB-T2 filter loads, Profile Any");
    old[kT2ProfileParam] = 1;
    const std::string txt = filterText(DvbSystem::T2, old);
    ModeFilter back = openFilter();
    filterFromText(DvbSystem::T2, txt, back);
    CHECK(txt == "4,3,0,6,1,0,0,1,1" && back == old, "the filter text round-trips: %s", txt.c_str());
    ModeFilter bad = openFilter();
    filterFromText(DvbSystem::T2, "99,-5,x,0", bad);
    CHECK(bad[0] == -1 && bad[1] == -1 && bad[3] == 0 && bad[kT2ProfileParam] == -1, "out-of-range values read as Any");
    ModeFilter s2 = openFilter();
    filterFromText(DvbSystem::S2, "1,2,0,1,2", s2);
    CHECK(filterText(DvbSystem::S2, s2) == "1,2,0,1,2", "a DVB-S2 filter as before");
}

int main() {
    testCounts();
    testValues();
    testFit();
    testAvailable();
    testLite();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("dvb_modes: all checks passed\n");
    return 0;
}
