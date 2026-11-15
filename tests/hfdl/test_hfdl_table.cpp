// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HFDL ground-station table consistency: no duplicate station ids or frequencies inside a station, every frequency in the HF aeronautical
// range (2.8 - 22 MHz) and inside a band of the band table, at most 20 per station (the width of the squitter mask), the stations and
// frequencies of the UDXF "Digital aero" list (HFDL table version 51) present, and the count of aircraft with a position.
#include "dect2/hfdl/hfdl_proto.h"
#include "dect2/hfdl/hfdl_hop.h"
#include "dect2/hfdl/hfdl_tel.h"
#include <cstdio>
#include <set>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

struct Ref { int id; std::vector<int> khz; };
// UDXF Digital-aero.pdf, HFDL table version 51 (station 15 left out of the frequency check: the dumphfdl table version 52 lists other frequencies for it)
static const Ref kRef[] = {
    {1, {5508, 6559, 8927, 10081, 11327, 13276, 17919, 21934}},
    {2, {5514, 6565, 8912, 8936, 10027, 11312, 11348, 13276, 13312, 13324, 17919, 21937}},
    {3, {3900, 5720, 6712, 8977, 11184, 15025, 17985}},
    {4, {5652, 6661, 8912, 11387, 13276, 17919, 21931}},
    {5, {5583, 6535, 8921, 10084, 13351, 17916}},
    {6, {5655, 6535, 8825, 10066, 13270, 17928, 21949}},
    {7, {2998, 3455, 5547, 6532, 8843, 8942, 10081, 11384}},
    {8, {3016, 4681, 5529, 8834, 11321, 13321, 17922, 21949}},
    {9, {2944, 2992, 3007, 3497, 4654, 4687, 5529, 5538, 5544, 6646, 8927, 8936, 10027, 10093, 11354, 17919, 17934, 21928, 21937}},
    {10, {2941, 5502, 6619, 8939, 10060, 13342, 17958, 21931}},
    {11, {5589, 6589, 8894, 10063, 13264, 17901}},
    {13, {4660, 6628, 8957, 11318, 13315, 17916, 21997}},
    {14, {5622, 6596, 8886, 10087, 13321, 17912, 21990}},
    {15, {}},
    {16, {5451, 6652, 8927, 11306, 13312, 17919, 21928}},
    {17, {6529, 8948, 11348, 13303, 17928, 21955}},
};

int main() {
    const auto& st = hfdlStations();
    std::set<int> ids;
    for (const auto& s : st) {
        CHECK(ids.insert(s.id).second);
        CHECK(s.id >= 1 && s.id <= 127);
        CHECK(!s.khz.empty() && s.khz.size() <= 20);
        std::set<int> f;
        for (int k : s.khz) {
            CHECK(f.insert(k).second);
            if (k < 2800 || k > 22000) { printf("out of range: %d kHz (gs %d)\n", k, s.id); fails++; }
            if (hfdlBandOfKhz(k) < 0) { printf("not in a band: %d kHz (gs %d)\n", k, s.id); fails++; }
        }
    }
    for (const auto& r : kRef) {
        const HfdlStationInfo* s = hfdlStation(r.id);
        CHECK(s != nullptr);
        if (!s) continue;
        for (int k : r.khz) {
            bool has = false; for (int x : s->khz) has |= x == k;
            if (!has) { printf("missing: %d kHz at gs %d\n", k, r.id); fails++; }
        }
    }
    {   // the mask bits follow the table order, every entry reaches the C++ side
        const HfdlStationInfo* b = hfdlStation(9);
        CHECK(b && b->khz.size() == 19);
        CHECK(b && hfdlMaskKhz(9, 1u << 18).size() == 1 && hfdlMaskKhz(9, 1u << 18)[0] == 2944);
    }
    {
        std::vector<HfdlAircraft> v(5);
        CHECK(hfdlCountWithPos(v) == 0);
        v[1].hasPos = true; v[4].hasPos = true;
        CHECK(hfdlCountWithPos(v) == 2);
        CHECK(hfdlCountWithPos({}) == 0);
    }
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
