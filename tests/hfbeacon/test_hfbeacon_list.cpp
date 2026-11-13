// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF beacon list: the shipped default against its sources (sigidwiki Long Dashes, priyom single-letter beacons, the NCDXF/IARU schedule text), the
// JSON round trip and refusals, the NCDXF slot arithmetic, and the planning of radio centres.
#include "dect2/hfbeacon/hfbeacon_list.h"
#include "dect2/geo/maidenhead.h"
#include <cmath>
#include <cstdio>
#include <set>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const auto d = hfb::defaultBeacons();
    int nc = 0, nl = 0, nd = 0, unc = 0;
    for (const auto& b : d) { if (b.kind == hfb::Ncdxf) nc++; else if (b.kind == hfb::Letter) nl++; else if (b.kind == hfb::Dashes) nd++; if (b.uncertain) unc++; }
    CHECK(nc == 90 && nl == 72 && nd == 8, "counts %d %d %d", nc, nl, nd);
    CHECK(unc == 8 + 3, "uncertain %d", unc);   // the 8 Long Dashes places and the three 7039 kHz markers

    // Long Dashes: the eight frequencies of sigidwiki, descending order in the cycle
    const double ld[8] = {29996, 27928, 21899, 16174, 12494, 11999, 8999, 6496};
    int k = 0;
    for (const auto& b : d) if (b.kind == hfb::Dashes) { CHECK(b.khz == ld[k], "dash %d %.0f", k, b.khz); k++; }

    // single letters: markers D P S C A F K M ascend by 100 Hz in each cluster, first values of the page
    const double first[9] = {3594.7, 4557.7, 5153.7, 7508.7, 8494.7, 10871.7, 13527.7, 16331.7, 20047.7};
    const char mk[9] = "DPSCAFKM";
    int c = 0, i = 0;
    for (const auto& b : d) {
        if (b.kind != hfb::Letter) continue;
        CHECK(b.letter == mk[i], "marker %c at cluster %d slot %d", b.letter, c, i);
        const double want = (c == 3 && i >= 5) ? 7039.2 + 0.1 * (i - 5) : first[c] + 0.1 * i;
        CHECK(std::fabs(b.khz - want) < 1e-6, "%s %.3f vs %.3f", b.name.c_str(), b.khz, want);
        CHECK(b.uncertain == (c == 3 && i >= 5), "uncertain flag %s", b.name.c_str());
        if (++i == 8) { i = 0; c++; }
    }
    // Moscow marker C is at Moscow, Vladivostok marker F in the far east
    for (const auto& b : d) if (b.kind == hfb::Letter && b.letter == 'C') { CHECK(std::fabs(b.lat - 55.75) < 0.1 && std::fabs(b.lon - 37.62) < 0.1, "Moscow"); break; }

    // NCDXF: 18 beacons x 5 bands from the five frequencies; the grid centres are on the right continents
    std::set<std::string> calls;
    for (const auto& b : d) if (b.kind == hfb::Ncdxf) calls.insert(hfb::kNcdxfCall[b.ncdxfIdx]);
    CHECK(calls.size() == 18, "%zu beacons", calls.size());
    for (const auto& b : d)
        if (b.kind == hfb::Ncdxf && b.ncdxfBand == 0) {
            if (b.ncdxfIdx == 0) CHECK(b.lat > 40 && b.lat < 41.5 && b.lon < -73 && b.lon > -75, "4U1UN at %.2f %.2f", b.lat, b.lon);
            if (b.ncdxfIdx == 5) CHECK(b.lat < -31 && b.lat > -33 && b.lon > 116 && b.lon < 118, "VK6RBP at %.2f %.2f", b.lat, b.lon);
        }

    // the published schedule: every 10 s one beacon per band, 3 minute cycle, 4U1UN first on 14.100 at :00, and each beacon "steps to the next
    // higher band" as the next beacon starts on the band below
    for (int band = 0; band < 5; band++) {
        std::set<int> seen;
        for (int s = 0; s < 18; s++) {
            const int bk = hfb::ncdxfBeaconAt(band, 1000000 * 180.0 + s * 10 + 3.3);
            seen.insert(bk);
            CHECK(hfb::ncdxfSlotAt(1000000 * 180.0 + s * 10 + 3.3) == s, "slot");
            if (band + 1 < 5) CHECK(hfb::ncdxfBeaconAt(band + 1, 1000000 * 180.0 + s * 10 + 13.3) == bk, "band %d slot %d: not the same beacon on the next band 10 s later", band, s);
            if (band > 0) CHECK(hfb::ncdxfBeaconAt(band, 1000000 * 180.0 + s * 10 + 1) == hfb::ncdxfBeaconAt(0, 1000000 * 180.0 + (s - band + 18) % 18 * 10 + 1), "band offset");
        }
        CHECK(seen.size() == 18, "band %d: %zu beacons in a cycle", band, seen.size());
    }
    CHECK(hfb::ncdxfBeaconAt(0, 0.5) == 0 && hfb::ncdxfBeaconAt(0, 10.5) == 1 && hfb::ncdxfBeaconAt(1, 10.5) == 0 && hfb::ncdxfBeaconAt(4, 40.5) == 0, "first slots");
    CHECK(hfb::ncdxfBeaconAt(0, 179.9) == 17 && hfb::ncdxfBeaconAt(0, 180.1) == 0, "cycle wrap");
    CHECK(hfb::ncdxfNextStart(3, 2, 7.0) == 50.0 && hfb::ncdxfNextStart(3, 2, 55.0) == 230.0, "next start %.1f %.1f", hfb::ncdxfNextStart(3, 2, 7.0), hfb::ncdxfNextStart(3, 2, 55.0));

    // JSON round trip of the whole default list
    const std::string j = hfb::toJson(d);
    std::vector<hfb::Beacon> r;
    std::string err;
    CHECK(hfb::fromJson(j, r, &err), "parse: %s", err.c_str());
    CHECK(r.size() == d.size(), "size %zu", r.size());
    for (size_t n = 0; n < std::min(r.size(), d.size()); n++) {
        const auto &a = d[n], &b = r[n];
        CHECK(a.name == b.name && std::fabs(a.khz - b.khz) < 1e-3 && a.kind == b.kind && a.letter == b.letter && a.ncdxfIdx == b.ncdxfIdx && a.ncdxfBand == b.ncdxfBand &&
              a.dashIdx == b.dashIdx && std::fabs(a.lat - b.lat) < 1e-3 && std::fabs(a.lon - b.lon) < 1e-3 && a.uncertain == b.uncertain && a.source == b.source && a.schedule == b.schedule,
              "entry %zu differs (%s)", n, a.name.c_str());
    }
    // a hand-written list: unknown keys and values are skipped, defaults apply
    {
        const char* txt = "{\"note\": [1, {\"x\": 2}], \"beacons\": [ {\"name\": \"WWV\", \"mhz\": 10, \"pattern\": \"carrier\", \"lat\": 40.68, \"lon\": -105.04, \"extra\": \"1\"},"
                          " {\"khz\": 4557.9, \"pattern\": \"letter\", \"letter\": \"s\", \"search_hz\": 30} ]}";
        std::vector<hfb::Beacon> h;
        CHECK(hfb::fromJson(txt, h, &err) && h.size() == 2, "hand list: %s", err.c_str());
        if (h.size() == 2) {
            CHECK(h[0].khz == 10000 && h[0].kind == hfb::Carrier && hfb::searchOf(h[0]) == 150, "carrier");
            CHECK(h[1].letter == 'S' && h[1].name == "4557.9 kHz" && hfb::searchOf(h[1]) == 30, "letter %c %s", h[1].letter, h[1].name.c_str());
        }
    }
    // refusals leave the output alone
    const char* bad[] = {"", "[]", "{\"beacons\": 3}", "{\"beacons\": [{\"name\": \"x\"}]}", "{\"beacons\": [{\"khz\": 100, \"pattern\": \"ncdxf\"}]}",
                         "{\"beacons\": [{\"khz\": 100, \"pattern\": \"letter\", \"letter\": \"Z\"}]}", "{\"beacons\": [{\"khz\": 100, \"lat\": 200}]}", "{\"beacons\": [{\"khz\": 1", "{\"nothing\": []}"};
    for (const char* t : bad) {
        std::vector<hfb::Beacon> keep = d;
        CHECK(!hfb::fromJson(t, keep, &err) && keep.size() == d.size() && !err.empty(), "accepted: %s", t);
    }

    // bands
    CHECK(hfb::bandName(14100) == "20 m" && hfb::bandName(28200) == "10 m" && hfb::bandName(21150) == "15 m" && hfb::bandName(18110) == "17 m" && hfb::bandName(24930) == "12 m", "band names");
    CHECK(hfb::bandIndexOf(3594.7) == 5 && hfb::bandIndexOf(16332) == 6, "other bands");

    // planning: 8 MHz of span covers the 9 + 5 + 8 groups with a few centres, none on a beacon, all inside 80 % of the span
    for (double rate : {2e6, 10e6, 20e6}) {
        const auto p = hfb::planCentres(d, rate);
        std::set<int> cov;
        for (const auto& c : p) {
            for (int bi : c.beacons) {
                cov.insert(bi);
                const double off = std::fabs(d[(size_t)bi].khz * 1e3 - c.centerHz);
                CHECK(off < 0.4 * rate, "rate %.0f: beacon %d %.0f Hz from the centre", rate, bi, off);
                CHECK(off > 19e3, "rate %.0f: beacon %d on the centre spike (%.0f Hz)", rate, bi, off);
            }
        }
        CHECK(cov.size() == d.size(), "rate %.0f covers %zu of %zu", rate, cov.size(), d.size());
        printf("  rate %.0f Msps: %zu centres\n", rate / 1e6, p.size());
        if (rate == 20e6) CHECK(p.size() <= 3, "20 Msps needs %zu centres", p.size());
    }
    CHECK(hfb::planCentres(d, 10e6, hfb::Dashes).size() <= 4, "dash centres");
    printf(fails ? "hfbeacon list: %d FAILED\n" : "hfbeacon list: all passed\n", fails);
    return fails ? 1 : 0;
}
