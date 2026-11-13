// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Marine VHF channel plan: every number maps to the right frequencies in each region (ITU rule), duplex pairs are 4.6 MHz apart, the A / B variants,
// the regional plans, the lookups and the data file against the compiled table.
#include "dect2/marinevhf/marinevhf_channels.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
using namespace dect2::marinevhf;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static bool eq(double a, double b) { return std::fabs(a - b) < 1e-6; }

int main() {
    // ---- the data file parses clean and the compiled table is that file
    {
        std::vector<CsvRow> rows; std::vector<std::string> errs;
        const size_t n = marineVhfParseCsv(marineVhfCsvText(), rows, &errs);
        CHECK(errs.empty(), "csv errors: %s", errs.empty() ? "" : errs[0].c_str());
        CHECK(n == rows.size() && n > 200, "rows %zu", n);
        const std::string here = __FILE__;
        const size_t cut = here.rfind("/tests/");
        if (cut != std::string::npos) {
            std::ifstream f(here.substr(0, cut) + "/data/bandplans/marine_vhf_channels.csv", std::ios::binary);
            if (f) {
                std::stringstream ss; ss << f.rdbuf();
                CHECK(ss.str() == marineVhfCsvText(), "the compiled table is the file in data/bandplans");
            }
        }
        // bad rows are reported, not kept
        std::vector<CsvRow> r2; std::vector<std::string> e2;
        const size_t k = marineVhfParseCsv("regions;number\nXXX;16;;156.8;156.8;V;;25;a;b\nINT;99;;156.8;156.8;V;;25;a;b\nINT;16;;156.8;156.8;Q;;25;a;b\nINT;16;;156.8;156.8;V;;25;a\nINT;16;;156.800;156.800;V;d;25;n;p\n", r2, &e2);
        CHECK(k == 1 && e2.size() == 4, "bad rows: kept %zu, errors %zu", k, e2.size());
    }

    // ---- the ITU rule
    CHECK(eq(ituShipMhz(1), 156.050) && eq(ituShipMhz(16), 156.800) && eq(ituShipMhz(28), 157.400), "ITU 1, 16, 28");
    CHECK(eq(ituShipMhz(60), 156.025) && eq(ituShipMhz(70), 156.525) && eq(ituShipMhz(88), 157.425), "ITU 60, 70, 88");
    CHECK(ituShipMhz(29) == 0 && ituShipMhz(59) == 0 && ituShipMhz(89) == 0 && ituShipMhz(0) == 0, "numbers outside the plan");

    // ---- the ITU based regions: every number of the plan, with the right frequencies
    const std::set<int> duplexNums = [] { std::set<int> s{1, 2, 3, 4, 5, 7}; for (int n = 18; n <= 28; n++) s.insert(n); for (int n = 60; n <= 66; n++) s.insert(n); for (int n = 78; n <= 86; n++) s.insert(n); return s; }();
    for (Region r : {RegionInternational, RegionEurope, RegionBaltic}) {
        const char* rc = regionCode(r);
        for (int n = 1; n <= 88; n++) {
            if (n > 28 && n < 60) continue;
            if (n == 87 || n == 88) {
                const Channel* a = findChannel(r, n, 'A'); const Channel* b = findChannel(r, n, 'B');
                CHECK(a && b, "%s %d A and B", rc, n);
                if (!a || !b) continue;
                CHECK(eq(a->shipMhz, ituShipMhz(n)) && !a->duplex(), "%s %dA simplex on the ship frequency", rc, n);
                CHECK(eq(b->shipMhz, ituShipMhz(n) + 4.6) && !b->duplex() && b->use == Use::Ais, "%s %dB is AIS on the coast frequency", rc, n);
                continue;
            }
            const Channel* c = findChannel(r, n);
            CHECK(c != nullptr, "%s channel %d exists", rc, n);
            if (!c) continue;
            CHECK(eq(c->shipMhz, ituShipMhz(n)), "%s %d ship %.3f", rc, n, c->shipMhz);
            const bool dup = duplexNums.count(n) > 0;
            CHECK(c->duplex() == dup, "%s %d duplex flag %d", rc, n, (int)c->duplex());
            if (dup) CHECK(eq(c->coastMhz, ituShipMhz(n) + 4.6), "%s %d coast %.3f", rc, n, c->coastMhz);
            else CHECK(eq(c->coastMhz, c->shipMhz), "%s %d simplex", rc, n);
            CHECK(c->listenMhz().size() == (dup ? 2u : 1u), "%s %d listen list", rc, n);
        }
        const Channel* c16 = findChannel(r, 16); const Channel* c70 = findChannel(r, 70);
        CHECK(c16 && c16->has(kDistress | kCalling) && c16->voice() && eq(c16->shipMhz, 156.800), "%s 16", rc);
        CHECK(c70 && c70->use == Use::Dsc && !c70->voice() && eq(c70->shipMhz, 156.525), "%s 70 is DSC", rc);
        const Channel* c6 = findChannel(r, 6); const Channel* c13 = findChannel(r, 13);
        CHECK(c6 && c6->has(kSar) && c13 && c13->has(kBridge), "%s 6 SAR, 13 bridge to bridge", rc);
        const Channel* c75 = findChannel(r, 75); const Channel* c76 = findChannel(r, 76);
        CHECK(c75 && c76 && c75->use == Use::LongRangeAis && c76->use == Use::LongRangeAis && eq(c75->shipMhz, 156.775) && eq(c76->shipMhz, 156.825) && c75->powerW <= 1 && c75->has(kLowPower), "%s 75 / 76 long range AIS, low power", rc);
        CHECK(findChannel(r, 15) && findChannel(r, 15)->has(kLowPower) && findChannel(r, 17)->has(kLowPower), "%s 15 and 17 low power", rc);
    }
    // ---- the regions that differ
    {
        const Channel* b = findChannel(RegionEurope, 37);
        CHECK(b && eq(b->shipMhz, 157.850) && !b->duplex(), "Europe 37 (M) 157.850");
        CHECK(!findChannel(RegionInternational, 37), "no 37 in the international plan");
        const Channel* g = findChannel(RegionBaltic, 61);
        CHECK(g && g->name.find("Tallinn") != std::string::npos, "Baltic 61 is the Tallinn GOFREP channel");
        CHECK(findChannel(RegionBaltic, 60)->name.find("Helsinki") != std::string::npos && findChannel(RegionBaltic, 74)->name.find("Petersburg") != std::string::npos, "Baltic 60, 74");
        CHECK(findChannel(RegionInternational, 61)->name.find("Tallinn") == std::string::npos, "the international 61 is a plain port channel");
    }
    {   // USA
        const Region u = RegionUsa;
        const Channel* a = findChannel(u, 22, 'A');
        CHECK(a && eq(a->shipMhz, 157.100) && !a->duplex() && a->has(kGovernment | kSafetyBroadcast), "USA 22A 157.100 simplex Coast Guard");
        CHECK(!findChannel(u, 22), "USA has no plain 22");
        const Channel* d = findChannel(u, 20); const Channel* da = findChannel(u, 20, 'A');
        CHECK(d && d->duplex() && eq(d->shipMhz, 157.000) && eq(d->coastMhz, 161.600), "USA 20 duplex 157.000 / 161.600");
        CHECK(da && !da->duplex() && eq(da->shipMhz, 157.000), "USA 20A simplex 157.000");
        for (int n : {24, 25, 26, 27, 28, 84, 85, 86}) { const Channel* c = findChannel(u, n); CHECK(c && c->duplex() && eq(c->coastMhz, c->shipMhz + 4.6) && c->has(kPublic), "USA %d duplex public correspondence", n); }
        for (int n : {1, 3, 5, 7, 18, 19, 21, 23, 63, 65, 66, 78, 79, 80, 81, 82, 83}) { const Channel* c = findChannel(u, n, 'A'); CHECK(c && !c->duplex() && eq(c->shipMhz, ituShipMhz(n)), "USA %dA simplex", n); }
        for (int n : {2, 4, 60, 61, 62, 64}) CHECK(!findChannel(u, n) && !findChannel(u, n, 'A'), "USA does not use %d", n);
        CHECK(findChannel(u, 9)->has(kCalling), "USA 9 is a calling channel");
        CHECK(findChannel(u, 13)->has(kBridge) && findChannel(u, 13)->powerW == 1, "USA 13 bridge to bridge 1 W");
        CHECK(findChannel(u, 16)->has(kDistress), "USA 16");
        CHECK(findChannel(u, 87, 'B')->use == Use::Ais && eq(findChannel(u, 87, 'B')->shipMhz, 161.975) && eq(findChannel(u, 88, 'B')->shipMhz, 162.025), "USA 87B / 88B AIS");
        CHECK(findChannel(u, 87, 'A') && eq(findChannel(u, 87, 'A')->shipMhz, 157.375) && eq(findChannel(u, 88, 'A')->shipMhz, 157.425), "USA 87A / 88A");
    }
    {   // Canada
        const Region c = RegionCanada;
        CHECK(findChannel(c, 21, 'B') && eq(findChannel(c, 21, 'B')->shipMhz, 161.650) && !findChannel(c, 21, 'B')->duplex(), "Canada 21B 161.650 simplex on the coast half");
        CHECK(findChannel(c, 83, 'B') && eq(findChannel(c, 83, 'B')->shipMhz, 161.775), "Canada 83B");
        CHECK(findChannel(c, 61, 'A') && findChannel(c, 64, 'A') && eq(findChannel(c, 64, 'A')->shipMhz, 156.225), "Canada 61A, 64A");
        CHECK(findChannel(c, 22, 'A') && findChannel(c, 22, 'A')->purpose.find("Canadian") != std::string::npos, "Canada 22A is the CCG channel");
        CHECK(!findChannel(RegionUsa, 21, 'B') && !findChannel(RegionUsa, 61, 'A'), "the Canadian extras are not in the USA plan");
    }

    // ---- every region: unique designators, sorted, frequencies in the band, B variants are simplex on the coast frequency
    for (int ri = 0; ri < kRegionCount; ri++) {
        const Region r = (Region)ri;
        std::set<std::pair<int, char>> seen; int prevN = 0; char prevS = 0;
        CHECK(!channels(r).empty(), "%s has channels", regionCode(r));
        for (const Channel& c : channels(r)) {
            CHECK(seen.insert({c.number, c.suffix}).second, "%s duplicate %s", regionCode(r), c.label().c_str());
            CHECK(c.number > prevN || (c.number == prevN && c.suffix > prevS), "%s order at %s", regionCode(r), c.label().c_str());
            prevN = c.number; prevS = c.suffix;
            CHECK(c.shipMhz >= 156.025 - 1e-9 && c.coastMhz <= 162.025 + 1e-9, "%s %s in band", regionCode(r), c.label().c_str());
            CHECK(!c.duplex() || eq(c.coastMhz, c.shipMhz + 4.6), "%s %s pair spacing", regionCode(r), c.label().c_str());
            CHECK(!c.name.empty() && !c.purpose.empty(), "%s %s has text", regionCode(r), c.label().c_str());
            if (c.suffix == 'B') CHECK(!c.duplex() && eq(c.shipMhz, ituShipMhz(c.number) + 4.6), "%s %s is the coast half", regionCode(r), c.label().c_str());
            if (c.suffix == 'A') CHECK(!c.duplex() && eq(c.shipMhz, ituShipMhz(c.number)), "%s %s is the ship half", regionCode(r), c.label().c_str());
            CHECK(c.use == Use::Voice || !c.has(kDistress | kCalling) || c.use == Use::Dsc, "%s %s", regionCode(r), c.label().c_str());
        }
        Region back;
        CHECK(parseRegion(regionCode(r), back) && back == r, "region code round trip");
        CHECK(*regionName(r) != 0, "region name");
    }
    {   Region x; CHECK(parseRegion("usa", x) && x == RegionUsa && !parseRegion("zz", x), "region parse"); }

    // ---- lookups
    {
        CHECK(findLabel(RegionInternational, "16") == findChannel(RegionInternational, 16), "label 16");
        CHECK(findLabel(RegionInternational, "Ch 16") == findChannel(RegionInternational, 16), "label Ch 16");
        CHECK(findLabel(RegionInternational, "VHF ch.16") == findChannel(RegionInternational, 16), "label VHF ch.16");
        CHECK(findLabel(RegionInternational, "channel 06") == findChannel(RegionInternational, 6), "label channel 06");
        CHECK(findLabel(RegionUsa, "22a") == findChannel(RegionUsa, 22, 'A'), "label 22a");
        CHECK(findLabel(RegionUsa, "22") == findChannel(RegionUsa, 22, 'A'), "22 where only 22A exists");
        CHECK(findLabel(RegionUsa, "20") == findChannel(RegionUsa, 20), "20 is the duplex one");
        CHECK(findLabel(RegionInternational, "87b")->use == Use::Ais, "label 87b");
        CHECK(!findLabel(RegionInternational, "") && !findLabel(RegionInternational, "abc") && !findLabel(RegionInternational, "16C") && !findLabel(RegionInternational, "99") && !findLabel(RegionInternational, "30"), "bad labels");
        CHECK(findChannel(RegionUsa, 22, 'A')->label() == "22A" && findChannel(RegionInternational, 16)->label() == "16", "label()");
        CHECK(findChannel(RegionInternational, 16)->title() == "16 Distress, safety, calling", "title(): %s", findChannel(RegionInternational, 16)->title().c_str());

        bool coast = true;
        const Channel* c = findFrequency(RegionInternational, 156.800, 0.0005, &coast);
        CHECK(c && c->number == 16 && !coast, "156.800 is 16");
        c = findFrequency(RegionInternational, 160.650, 0.0005, &coast);
        CHECK(c && c->number == 1 && coast, "160.650 is the coast side of 1");
        c = findFrequency(RegionInternational, 156.050, 0.0005, &coast);
        CHECK(c && c->number == 1 && !coast, "156.050 is the ship side of 1");
        c = findFrequency(RegionInternational, 161.975, 0.0005, &coast);
        CHECK(c && c->number == 87 && c->suffix == 'B' && !coast, "161.975 is 87B (AIS 1)");
        c = findFrequency(RegionInternational, 162.025);
        CHECK(c && c->number == 88 && c->suffix == 'B', "162.025 is 88B");
        c = findFrequency(RegionUsa, 157.000);
        CHECK(c && c->number == 20 && c->suffix == 0, "USA 157.000 is 20");
        c = findFrequency(RegionUsa, 161.600, 0.0005, &coast);
        CHECK(c && c->number == 20 && coast, "USA 161.600 is the coast side of 20");
        c = findFrequency(RegionCanada, 161.650);
        CHECK(c && c->number == 21 && c->suffix == 'B', "Canada 161.650 is 21B");
        CHECK(!findFrequency(RegionInternational, 156.8125) && !findFrequency(RegionInternational, 150.0), "frequencies off the channels");
        CHECK(findFrequency(RegionInternational, 156.8004, 0.0005) && !findFrequency(RegionInternational, 156.8004, 0.0001), "tolerance");

        const auto vf = voiceFrequencies(RegionInternational);
        const auto af = allFrequencies(RegionInternational);
        CHECK(!vf.empty() && vf.front() > 156.0 && vf.back() <= 162.025, "voice frequency range");
        CHECK(af.size() > vf.size(), "all frequencies include DSC and AIS");
        bool has70 = false, has16 = false, hasAis = false;
        for (double f : vf) { if (eq(f, 156.525)) has70 = true; if (eq(f, 156.800)) has16 = true; if (eq(f, 161.975)) hasAis = true; }
        for (double f : af) if (eq(f, 161.975)) hasAis = true;
        CHECK(!has70 && has16 && hasAis, "voice list: 16 yes, 70 no; all list: AIS yes");
        for (size_t i = 1; i < vf.size(); i++) CHECK(vf[i] > vf[i - 1] + 1e-6, "voice frequencies ascending and distinct");
    }

    // ---- up / down
    {
        const Region r = RegionInternational;
        const Channel* c = findChannel(r, 16);
        CHECK(stepChannel(r, c, +1)->number == 17 && stepChannel(r, c, -1)->number == 15, "16 up and down");
        CHECK(stepChannel(r, findChannel(r, 69), +1)->number == 71, "69 up skips DSC channel 70");
        CHECK(stepChannel(r, findChannel(r, 71), -1)->number == 69, "71 down skips 70");
        CHECK(stepChannel(r, findChannel(r, 74), +1)->number == 77, "74 up skips the long range AIS channels");
        CHECK(stepChannel(r, findChannel(r, 86), +1)->number == 87 && stepChannel(r, findChannel(r, 86), +1)->suffix == 'A', "86 up is 87A");
        CHECK(stepChannel(r, findChannel(r, 88, 'A'), +1)->number == 1, "88A up wraps to 1");
        CHECK(stepChannel(r, findChannel(r, 1), -1)->number == 88, "1 down wraps to 88A");
        CHECK(stepChannel(r, nullptr, +1)->number == 1 && stepChannel(r, nullptr, -1)->number == 88, "no current: first / last");
        CHECK(stepChannel(r, findChannel(r, 70), +1)->number == 71 && stepChannel(r, findChannel(r, 70), -1)->number == 69, "from the DSC channel itself");
        CHECK(stepChannel(RegionUsa, findChannel(RegionUsa, 20), +1)->suffix == 'A' && stepChannel(RegionUsa, findChannel(RegionUsa, 20), +1)->number == 20, "USA 20 up is 20A");
        // a full lap visits every voice channel once
        const Channel* s = findChannel(r, 1); int n = 0;
        do { s = stepChannel(r, s, +1); n++; } while (s->number != 1 && n < 500);
        int voice = 0; for (const Channel& q : channels(r)) if (q.voice()) voice++;
        CHECK(n == voice, "a lap is %d steps, %d voice channels", n, voice);
    }

    // ---- channel numbers from the text of a DSC call
    CHECK(channelFromDscText("VHF ch 16") == 16, "dsc 16");
    CHECK(channelFromDscText("VHF ch 6 (ship simplex)") == 6, "dsc 6");
    CHECK(channelFromDscText("VHF ch 1006 (coast simplex)") == 6, "dsc 1006");
    CHECK(channelFromDscText("VHF ch 88") == 88 && channelFromDscText("VHF ch 0") == 0 && channelFromDscText("VHF ch 89") == 0, "dsc range");
    CHECK(channelFromDscText("MF/HF channel 12345") == 0 && channelFromDscText("8291.0 kHz") == 0 && channelFromDscText("") == 0, "dsc: not VHF");
    CHECK(channelFromDscText("72") == 72, "dsc bare number");

    if (fails) printf("%d failures\n", fails); else printf("marinevhf channels: all ok\n");
    return fails ? 1 : 0;
}
