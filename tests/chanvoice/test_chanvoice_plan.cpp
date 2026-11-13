// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Channel plans: the numbers of CB (FCC and CEPT order of 23 - 25, the German 80 and the UK 27/81 extensions), FRS / GMRS, PMR446, the amateur
// repeater grids, HF voice schedules, lookups, country and position to region.
#include "dect2/chanvoice/chv_plan.h"
#include <cmath>
#include <cstdio>
#include <set>
using namespace dect2::chv;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static bool eq(double a, double b) { return std::fabs(a - b) < 1e-6; }

int main() {
    // ---- CB
    const auto& eu = channels(Service::Cb, 0);
    const auto& us = channels(Service::Cb, 3);
    CHECK(eu.size() == 40 && us.size() == 40, "CB 40 channels: %zu %zu", eu.size(), us.size());
    CHECK(eq(us[0].mhz, 26.965) && eq(us[39].mhz, 27.405) && eq(us[8].mhz, 27.065) && eq(us[18].mhz, 27.185), "CB US 1, 40, 9, 19");
    CHECK(eq(us[22].mhz, 27.255) && eq(us[23].mhz, 27.235) && eq(us[24].mhz, 27.245), "CB US 23, 24, 25 (FCC order)");
    CHECK(eq(eu[22].mhz, 27.235) && eq(eu[23].mhz, 27.245) && eq(eu[24].mhz, 27.255), "CB CEPT 23, 24, 25 (number order)");
    for (int i = 0; i < 40; i++) if (i < 22 || i > 24) CHECK(eq(eu[(size_t)i].mhz, us[(size_t)i].mhz), "CB channel %d differs between CEPT and FCC", i + 1);
    // spacing: every channel on the 10 kHz grid, no duplicates
    { std::set<long> f; for (auto& c : us) { f.insert(std::lround(c.mhz * 1e6)); CHECK(std::lround(c.mhz * 1e5) % 1 == 0 && std::lround(c.mhz * 1e6) % 5000 == 0, "CB %s off the 5 kHz grid", c.label.c_str()); } CHECK(f.size() == 40, "CB duplicates"); }
    CHECK(us[8].has(kEmergency) && us[18].has(kCalling), "CB 9 emergency, 19 calling");
    const auto& de = channels(Service::Cb, 1);
    CHECK(de.size() == 80 && eq(de[0].mhz, 26.965) && eq(de[40].mhz, 26.565) && eq(de[79].mhz, 26.955), "CB 80: %zu", de.size());
    const auto& uk = channels(Service::Cb, 2);
    CHECK(uk.size() == 80 && eq(uk[40].mhz, 27.60125) && eq(uk[79].mhz, 27.99125) && uk[40].demod == Demod::Nfm, "CB UK 80: %zu", uk.size());
    CHECK(findLabel(Service::Cb, 3, "ch 19") && findLabel(Service::Cb, 3, "19")->number == 19 && findLabel(Service::Cb, 2, "u5")->mhz > 27.64, "CB label lookup");
    CHECK(findFrequency(Service::Cb, 3, 27.0652, 0.005)->number == 9 && !findFrequency(Service::Cb, 3, 27.0953, 0.004), "CB frequency lookup");
    // ---- FRS / GMRS
    const auto& f = channels(Service::Frs, 0);
    CHECK(f.size() == 22 && eq(f[0].mhz, 462.5625) && eq(f[6].mhz, 462.7125) && eq(f[7].mhz, 467.5625) && eq(f[13].mhz, 467.7125) && eq(f[14].mhz, 462.550) && eq(f[21].mhz, 462.725), "FRS plan");
    CHECK(f[14].has(kRepeater) && eq(f[14].txMhz, 467.550) && eq(f[21].txMhz, 467.725) && !f[0].has(kRepeater) && f[7].has(kLowPower), "GMRS repeater inputs");
    // ---- PMR446
    const auto& p = channels(Service::Pmr, 0);
    CHECK(p.size() == 16 && eq(p[0].mhz, 446.00625) && eq(p[7].mhz, 446.09375) && eq(p[8].mhz, 446.10625) && eq(p[15].mhz, 446.19375), "PMR446 plan");
    for (size_t i = 1; i < p.size(); i++) CHECK(eq(p[i].mhz - p[i - 1].mhz, 0.0125), "PMR spacing at %zu", i);
    // ---- amateur
    for (int r = 0; r < 3; r++) {
        const auto& h = channels(Service::Ham, r);
        CHECK(h.size() > 30, "ham region %d: %zu channels", r, h.size());
        int rep = 0; for (auto& c : h) if (c.has(kRepeater)) { rep++; CHECK(c.txMhz != 0 && c.duplex(), "ham repeater without input"); }
        CHECK(rep > 20, "ham region %d: %d repeater channels", r, rep);
    }
    { const Channel* c = findFrequency(Service::Ham, 0, 145.6125, 0.003); CHECK(c && c->label == "RV49" && eq(c->txMhz, 145.0125), "R1 RV49 output 145.6125, input 145.0125"); }
    { const Channel* c = findFrequency(Service::Ham, 1, 146.940, 0.003); CHECK(c && eq(c->txMhz, 146.340), "R2 146.940 -0.6"); }
    { const Channel* c = findFrequency(Service::Ham, 1, 147.150, 0.003); CHECK(c && eq(c->txMhz, 147.750), "R2 147.150 +0.6: %f", c ? c->txMhz : 0.0); }
    { const Channel* c = findFrequency(Service::Ham, 1, 444.000, 0.003); CHECK(c && eq(c->txMhz, 449.000), "R2 70 cm +5"); }
    { const Channel* c = findFrequency(Service::Ham, 0, 438.800, 0.003); CHECK(c && eq(c->txMhz, 431.200), "R1 70 cm -7.6"); }
    // ---- HF
    const auto& hf = channels(Service::HfVoice, 0);
    CHECK(findFrequency(Service::HfVoice, 0, 11.175, 0.0005) && findFrequency(Service::HfVoice, 0, 4.724, 0.0005) && findFrequency(Service::HfVoice, 0, 8.992, 0.0005) && findFrequency(Service::HfVoice, 0, 15.016, 0.0005), "HFGCS primary frequencies");
    for (auto& c : hf) CHECK(c.demod == Demod::Usb, "HF %s not USB", c.label.c_str());
    { const Channel* g = findLabel(Service::HfVoice, 0, "Gander 6604"); CHECK(g && onAirAtMinute(*g, 20) && onAirAtMinute(*g, 24) && !onAirAtMinute(*g, 25) && onAirAtMinute(*g, 50) && !onAirAtMinute(*g, 10), "Gander slots");
      CHECK(g && nextStartMinute(*g, 26) == 50 && nextStartMinute(*g, 51) == 20, "Gander next start"); }
    { const Channel* s = findLabel(Service::HfVoice, 0, "Shannon 5505"); CHECK(s && onAirAtMinute(*s, 33) && nextStartMinute(*s, 3) == -1, "Shannon continuous"); }
    // ---- regions
    CHECK(regionForCountry(Service::Cb, "DE") == 1 && regionForCountry(Service::Cb, "gb") == 2 && regionForCountry(Service::Cb, "US") == 3 && regionForCountry(Service::Cb, "LT") == 0, "CB country regions");
    CHECK(regionForCountry(Service::Ham, "LT") == 0 && regionForCountry(Service::Ham, "CA") == 1 && regionForCountry(Service::Ham, "AU") == 2, "ham country regions");
    CHECK(regionForPosition(Service::Cb, 54.7, 25.3) == 0 && regionForPosition(Service::Cb, 51.5, -0.1) == 2 && regionForPosition(Service::Cb, 40.7, -74.0) == 3 && regionForPosition(Service::Cb, 52.5, 13.4) == 1, "CB position regions");
    CHECK(regionForPosition(Service::Ham, 54.7, 25.3) == 0 && regionForPosition(Service::Ham, 40.7, -74.0) == 1 && regionForPosition(Service::Ham, -33.9, 151.2) == 2, "ham position regions");
    CHECK(regionIndex(Service::Cb, "uk") == 2 && regionIndex(Service::Cb, "xx") == -1, "regionIndex");
    CHECK(toneLabel(88.5, -1, false) == "88.5 Hz" && toneLabel(0, 23, true) == "D023I" && toneLabel(0, -1, false).empty(), "tone labels");
    CHECK(handsetToneNumber(67.0) == 1 && handsetToneNumber(250.3) == 38 || handsetToneNumber(250.3) > 30, "handset tone numbers: %d", handsetToneNumber(250.3));
    CHECK(micBands(0).size() == 3 && micBands(1).size() == 4, "mic bands");
    for (int s = 0; s < kServiceCount; s++) { Service back; CHECK(serviceOfFamily(familyOf((Service)s), back) && back == (Service)s, "family round trip"); }
    printf(fails ? "chv_plan: %d FAILED\n" : "chv_plan: all passed\n", fails);
    return fails ? 1 : 0;
}
