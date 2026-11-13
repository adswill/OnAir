// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Digital video generator, tab "Digital Television Mode Selector" (dect2/tv/dvb_modes.h), without a window:
//  1. the number of modes per system: those of DVB_transmission_modes.xlsx, the satellite ones once per roll-off
//  2. bandwidth, bitrate and C/N of modes of each system against the values the workbook computes (all 34 980 rows were compared
//     once when the tables were written; these keep them honest)
//  3. the mode names
//  4. the link fit: the bitrate, the bandwidth and the C/N each rule a mode out; satellite modes scale with the symbol rate
//  5. the available modes, the best first, and the filters
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
    const size_t want[] = {480, 28560, 5712, 5, 312, 714};
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

int main() {
    testCounts();
    testValues();
    testFit();
    testAvailable();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("dvb_modes: all checks passed\n");
    return 0;
}
