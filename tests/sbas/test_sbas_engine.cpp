// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SBAS mode end to end: the synthetic sky of the GNSS generator with only its three geostationary satellites (PRN 123, 127, 128), through the SBAS receiver
// (no GPS satellite to lock first): acquisition on the SBAS PRNs, tracking, Viterbi and CRC, and the content decoder. With a clean signal and with a
// frequency offset of +-5 kHz and a sample clock 20 ppm off (the faults of a cheap radio). The content is the generator's (sbas::synthData), so the decoded
// values can be checked against what was put in.
#include "dect2/sbas/sbas_engine.h"
#include "dect2/engine/test_parallel.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

using namespace dect2;
using dect2::testpar::CaseOut;
#define CHECK(c, ...) do { if (!(c)) out.fail(__LINE__, __VA_ARGS__); } while (0)

struct Case { const char* name; double cfoHz; double sroPpm; double secs; double cn0Top; };

static void runCase(const Case& k, CaseOut& out) {
    const double rate = 4e6;
    SynthConfig sc;
    sc.cfoHz = k.cfoHz;
    sc.sroPpm = k.sroPpm;
    sc.modeVal[3] = k.cn0Top;
    auto syn = makeSbasSynth(sc, rate);
    CHECK(syn != nullptr, "no generator");
    if (!syn) return;
    SbasReceiver rx;
    rx.configure(rate);
    rx.setCenterHz(1575.42e6);
    CHECK(rx.ready(), "receiver not ready at 4 Msps");
    const size_t chunk = 64000;
    std::vector<cf32> buf(chunk);
    SbasTelemetry t;
    uint64_t seq = 0;
    const size_t total = (size_t)(k.secs * rate);
    for (size_t done = 0; done < total; done += chunk) {
        syn->generate(buf.data(), chunk);
        rx.feed(buf.data(), chunk);
        SbasTelemetry u;
        if (rx.telemetry(u, seq)) { seq = u.seq; t = std::move(u); }
    }
    out.print("%s: %s\n", k.name, sbasSummary(t).c_str());
    CHECK(t.seq > 0, "no telemetry");
    std::set<int> want = {123, 127, 128};
    std::set<int> got;
    for (const auto& s : t.sats) got.insert(s.prn);
    CHECK(got == want, "tracked %zu satellites, not the three that are sent", got.size());
    CHECK(t.state == 2, "state %d", t.state);
    for (const auto& g : t.geos) {
        if (!want.count(g.prn)) continue;
        int types = 0;
        for (int b = 0; b < 32; b++) types += (g.typesSeen >> b) & 1;
        out.print("  PRN %d: %u messages, %d types, mask %d satellites, %d fast corrections\n", g.prn, g.messages, types, g.nMasked, sbas::Network::summarise(g).nFast);
        CHECK(g.messages >= (uint32_t)(k.secs * 0.45), "PRN %d: %u messages in %.0f s", g.prn, g.messages, k.secs);
        CHECK(g.badCrc == 0, "PRN %d: bad CRC counted %u", g.prn, g.badCrc);
        CHECK(g.maskValid && g.nMasked == 15, "PRN %d: PRN mask holds %d", g.prn, g.nMasked);
        CHECK(g.maskValid && g.iodp == 1, "PRN %d: IODP %d", g.prn, g.iodp);
        CHECK(g.maskValid && sbas::Network::slotPrn(g, 14) == g.prn, "PRN %d: slot 15 is %d", g.prn, sbas::Network::slotPrn(g, 14));
        CHECK(sbas::Network::summarise(g).nFast >= 38, "PRN %d: %d fast corrections", g.prn, sbas::Network::summarise(g).nFast);
        CHECK(g.nav.valid, "PRN %d: no GEO navigation message", g.prn);
        if (g.nav.valid) {
            const double r = std::sqrt(g.nav.pos[0] * g.nav.pos[0] + g.nav.pos[1] * g.nav.pos[1] + g.nav.pos[2] * g.nav.pos[2]);
            CHECK(std::fabs(r - 42164169.0) < 100.0, "PRN %d: GEO radius %.0f m", g.prn, r);
        }
        CHECK(g.utc.valid && g.utc.dtLS == 18, "PRN %d: time offset / leap seconds %d", g.prn, g.utc.dtLS);
        CHECK(g.almanac.size() == 3, "PRN %d: almanac of %zu", g.prn, g.almanac.size());
        const sbas::IonoSummary is = sbas::Network::ionoSummary(g, 0);
        CHECK(is.nIgp == 60, "PRN %d: band 0 holds %d grid points", g.prn, is.nIgp);
        CHECK(types >= 12, "PRN %d: %d message types", g.prn, types);
        for (int ty = 0; ty < 64; ty++) {
            static const int sent[] = {1, 2, 3, 4, 5, 7, 9, 10, 12, 17, 18, 24, 25, 26, 27, 28};
            bool ok = false;
            for (int s : sent) ok |= s == ty;
            CHECK(ok || !((g.typesSeen >> ty) & 1), "PRN %d: type %d was not sent", g.prn, ty);
        }
    }
    bool sawMask = false;
    for (const auto& l : t.log) sawMask |= l.type == 1 && l.text.find("PRN mask") != std::string::npos;
    CHECK(sawMask, "the viewer log has no PRN mask line (%zu entries)", t.log.size());
    CHECK(t.total >= (uint64_t)(3 * k.secs * 0.45), "%llu messages in all", (unsigned long long)t.total);
}

int main() {
    const Case cases[] = {
        {"clean", 0, 0, 45, 0},
        {"+5 kHz, sample clock +20 ppm", 5000, 20, 50, 0},
        {"-5 kHz", -5000, 0, 50, 0},
        {"weak: 38 dB-Hz at the zenith", 0, 0, 60, 38},
        {"sample clock -20 ppm (a radio without a TCXO: the carrier is 31 kHz off)", 0, -20, 100, 0},
    };
    const int fails = dect2::testpar::runCases(5, [&](size_t i, CaseOut& out) { runCase(cases[i], out); }, {4, 3, 1, 2, 0});
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("sbas engine: ok\n");
    return 0;
}
