// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDL2 session log (#309): all frames kept past the screen's 400, UTC date and time on each, CSV / JSON / text export.
#include "../../app/vdl2/vdl2_log.h"
#include <cstdio>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static bool has(const std::string& s, const char* n) { return s.find(n) != std::string::npos; }
int main() {
    dect2::Vdl2Telemetry t;
    auto add = [&](uint64_t serial) {
        dect2::Vdl2Message m; m.serial = serial; m.wallTime = 1790000000 + (int64_t)serial; m.freqHz = 136.975e6; m.src = 0x4CA2B1; m.dst = 0x123456;
        m.srcType = 1; m.dstType = 4; m.frameName = "I"; m.reg = "EI-ABC"; m.summary = "hello, \"world\""; m.acars = true; m.label = "H1"; m.acarsText = "TEXT";
        t.messages.insert(t.messages.begin(), m);
    };
    vdl2log::Log L;
    for (uint64_t i = 1; i <= 3; i++) add(i);
    CHECK(L.feed(t) == 3 && L.size() == 3);
    CHECK(L.feed(t) == 0 && L.size() == 3);                         // the same snapshot twice adds nothing
    for (uint64_t i = 4; i <= 600; i++) add(i);
    t.messages.resize(400);                                          // the screen keeps 400: serial 201..600
    CHECK(L.feed(t) == 400 && L.size() == 403);                      // frames 4..200 fell off the screen unseen: why the log is fed every frame
    CHECK(vdl2log::utc(1790000001) == "2026-09-21 14:13:21");
    CHECK(vdl2log::utc(0).empty());
    const std::string csv = L.render(vdl2log::Fmt::Csv);
    CHECK(has(csv, "utc,freq_mhz") && has(csv, "4CA2B1") && has(csv, "\"hello, \"\"world\"\"\"") && has(csv, "136.975"));
    const std::string js = L.render(vdl2log::Fmt::Json);
    CHECK(has(js, "\"utc\"") && has(js, "\"registration\": \"EI-ABC\""));
    const std::string tx = L.render(vdl2log::Fmt::Text);
    CHECK(has(tx, " UTC  136.975 MHz") && has(tx, "4CA2B1 -> 123456") && has(tx, "ACARS H1: TEXT"));
    std::printf(fails ? "vdl2_log FAILED\n" : "vdl2_log ok\n");
    return fails ? 1 : 0;
}
