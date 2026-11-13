// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SUBSET-026 chapter 7 packets from the openETCS XML transcription (no conformance test vectors exist publicly). Simulated user bits only.
#include "dect2/ebicab/ebicab_proto.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2::ebicab;
#include "../common/test_util.h"
#undef CHECK
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

struct W {
    std::vector<uint8_t> u;
    void put(int v, int n) { for (int i = n - 1; i >= 0; i--) u.push_back((uint8_t)((v >> i) & 1)); }
};

static void hdr(W& w) { for (int i = 0; i < 50; i++) w.u.push_back(0); }
static void pack(W& w, int nid, const std::vector<std::pair<int,int>>& body) {
    int total = 23; for (auto& b : body) total += b.second;
    w.put(nid, 8); w.put(1, 2); w.put(total, 13);
    for (auto& b : body) w.put(b.first, b.second);
}

int main() {
    W w; hdr(w);
    pack(w, 5, {{1,2},{120,15},{0,1},{7,14},{0,1},{1,2},{10,6},{1,5},{55,15},{1,1},{246,10},{9,14},{0,1},{1,2},{10,6}});
    pack(w, 21, {{1,2},{100,15},{1,1},{20,8},{1,5},{300,15},{0,1},{255,8}});
    pack(w, 27, {{2,2},{50,15},{30,7},{0,1},{1,5},{0,2},{3,4},{20,7},{1,5},{200,15},{25,7},{0,1},{0,5}});
    pack(w, 41, {{1,2},{32767,15},{3,3},{0,5}});
    pack(w, 65, {{1,2},{9,8},{800,15},{400,15},{1,1},{8,7}});
    std::vector<std::pair<int,int>> tx = {{1,2},{0,2},{0,1},{0,15},{0,4},{0,3},{0,15},{0,10},{0,4},{0,3},{0,2},{3,8},{'H',8},{'I',8},{'!',8}};
    pack(w, 72, tx);
    pack(w, 79, {{1,2},{0,1},{7,14},{100,15},{1,1},{12345,24},{0,5}});
    w.put(255, 8);
    w.u.resize(830, 0);
    auto p = parsePackets(w.u);
    CHECK(p.size() == 7);
    if (p.size() == 7) {
        CHECK(p[0].text == "linking: next group 7 at 120.0 m, 9 at +55.0 m");
        CHECK(p[1].text == "gradient: at 100.0 m +20 per mil, +300.0 m end");
        CHECK(p[2].text == "speed: 150 km/h at 500.0 m, 125 km/h at +2000.0 m");
        CHECK(p[3].text == "level transition to level 2 at now/unknown");
        CHECK(p[4].text == "temporary speed 9: 40 km/h at 800.0 m for 400.0 m");
        CHECK(p[5].text == "text: HI!");
        CHECK(p[6].text == "geographical position: group 7 100.0 m from it, km 12.345 counting up");
        CHECK(p[1].qDir == 1 && p[1].lenBits > 23);
    }
    // an unknown packet is skipped by its length, the next is read
    W v; hdr(v); pack(v, 44, {{1234,16},{5,8}}); pack(v, 65, {{1,2},{1,8},{10,15},{10,15},{0,1},{20,7}}); v.put(255, 8); v.u.resize(210, 0);
    p = parsePackets(v.u);
    CHECK(p.size() == 2 && p[0].nid == 44 && p[1].nid == 65);
    // a length running past the end stops the walk
    W x; hdr(x); x.put(5, 8); x.put(0, 2); x.put(2000, 13); x.u.resize(210, 0);
    CHECK(parsePackets(x.u).empty());
    return testSummary("ebicab_packets");
}
