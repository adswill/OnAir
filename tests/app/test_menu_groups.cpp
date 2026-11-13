// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Tools menu's grouping table (app/ui/menu_groups.h): every item of the old flat menu appears exactly once, every group is a real
// group, the search matches, and the Mode menu's chunks cover a category exactly with no chunk over the limit.
#include "ui/menu_groups.h"
#include <cstdio>
#include <map>
#include <set>
#include <string>
using namespace menugroups;
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

// the items of the flat Tools menu before 4.0 (ui2_shell.cpp)
static const char* const kOld[] = {
    "Transcripts ...",
    "Passes",
    "Scheduler...",
    "Recordings...",
    "SatNOGS transmitters",
    "Direction finder",
    "Interference hunter",
    "Passive radar",
    "Images",
    "Data feeds...",
    "EchoLink...",
    "Wireshark...",
    "LAN swarm...",
    "Messages...",
    "Protocol lab...",
    "nRF24 / ESP32 radio...",
    "Signal Lab...",
    "Mode Builder...",
    "TX studio...",
    "Digital video generator...",
    "Integrations...",
    "Conversations...",
    "Watchlists...",
    "Automations...",
    "Plugins...",
    "Backup and restore...",
    "Memory...",
    "Datasets...",
    "Data packs...",
    "Frequencies...",
    "Show Frequencies as a tab",
    "Transmit...",
    "Map",
    "Sweep scanner",
    "Station health",
    "Search everything heard",
    "Insights",
    "My station",
    "Reception doctor",
    "RF fingerprint",
    "Everything in view",
    "Multi-radio trunking",
    "Scanner (highly experimental)",
    "Propagation...",
    "Antenna designer...",
    "Antenna rotator...",
    "Voice keys...",
    "Calibrate frequency..."
};

int main() {
    int n = 0;
    const ToolItem* it = toolItems(n);
    std::map<std::string, int> seen;
    std::set<int> ids;
    std::map<int, int> perGroup;
    for (int i = 0; i < n; i++) {
        seen[it[i].label]++;
        CHECK(ids.insert((int)it[i].id).second, "tool id %d used twice", (int)it[i].id);
        CHECK(it[i].group >= kTopLevel && it[i].group < kNumToolGroups, "%s: group %d out of range", it[i].label, it[i].group);
        perGroup[it[i].group]++;
    }
    for (const char* l : kOld) CHECK(seen[l] == 1, "\"%s\" appears %d times in the table (want 1)", l, seen[l]);
    CHECK(seen["Plugin store..."] == 1, "Plugin store entry missing");
    CHECK(n == (int)(sizeof kOld / sizeof kOld[0]) + 1, "table has %d items, the old menu %d + the new Plugin store", n, (int)(sizeof kOld / sizeof kOld[0]));
    for (int g = 0; g < kNumToolGroups; g++) {
        CHECK(perGroup[g] >= 2, "group %d (%s) has %d items: no group with one item", g, toolGroupTitle(g), perGroup[g]);
        CHECK(perGroup[g] <= 9, "group %d (%s) has %d items", g, toolGroupTitle(g), perGroup[g]);
        CHECK(*toolGroupTitle(g), "group %d has no title", g);
    }
    CHECK(perGroup[kTopLevel] >= 3 && perGroup[kTopLevel] <= 6, "top level has %d items", perGroup[kTopLevel]);
    CHECK(matchesQuery("Interference hunter", "HUNT") && matchesQuery("Map", "") && !matchesQuery("Map", "zzz"), "search");
    // chunks: any size from 1 to 400 is covered once, in order, within the limit
    for (int sz = 1; sz <= 400; sz++) {
        int next = 0;
        const int c = chunkCount(sz);
        for (int i = 0; i < c; i++) {
            int b, e; chunkRange(sz, i, b, e);
            CHECK(b == next && e > b && e - b <= kMaxPerSubmenu, "size %d chunk %d: [%d,%d)", sz, i, b, e);
            next = e;
        }
        CHECK(next == sz, "size %d: chunks end at %d", sz, next);
        CHECK(sz > kMaxPerSubmenu || c == 1, "size %d split without need", sz);
    }
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("menu groups OK: %d tools\n", n);
    return 0;
}
