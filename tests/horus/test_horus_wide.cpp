// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Project Horus: three balloons in one capture (Binary v2 on the dial, Binary v1 12 kHz above, RTTY 18 kHz below) at a radio rate
// with the dial off the centre and a frequency error; the wide-span search must open channels for the other two. Then the payload
// table (track, climb rate), the results file round trip, the user tables and the mode numbers.
#include "dect2/horus/horus_gen.h"
#include "dect2/horus/horus_rx.h"
#include "../../core/src/engine/engine_internal.h"
#include "../common/test_util.h"
#include <cmath>
#include <string>
using namespace dect2;
using namespace dect2::horus;

int main() {
    const auto tu = horusTuning();
    CHECK(tu.stdMode == kStandard && activeStdFor(tu.stdMode) == kActiveStandard && std::string(tu.id) == "horus", "mode numbers");
    CHECK(matchesMode(kFamily, kActiveStandard) && !matchesMode(kFamily, kActiveStandard - 1) && !matchesMode(kFamily - 1, kActiveStandard), "a stale snapshot of another mode is not shown");

    const double rate = 192000;
    SynthConfig sc;
    sc.snrDb = 8; sc.cfoHz = 1500; sc.modeOpt[0] = 5; sc.modeOpt[3] = 9;
    auto g = makeHorusSynth(sc, rate);
    CHECK(g != nullptr && synthDialHz(rate) == -tu.tuneOffsetHz, "generator at a radio rate");
    HorusReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(synthDialHz(rate));
    rx.setCenterHz(434.650e6);
    std::vector<cf32> buf(9600);
    for (int i = 0; i < (int)(60 * rate / buf.size()); i++) { g->generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    HorusTelemetry t;
    CHECK(rx.telemetry(t, 0), "telemetry");
    auto find = [&](const char* c) -> const HorusPayload* { for (const auto& p : t.payloads) if (p.callsign == c) return &p; return nullptr; };
    const HorusPayload* v2 = find("4FSKTEST-V2");
    const HorusPayload* v1 = find("ID 0");
    const HorusPayload* rt = find("ONAIR1");
    CHECK(v2 && v2->packets >= 12, "v2 on the dial: %d packets", v2 ? (int)v2->packets : 0);
    CHECK(v1 && v1->packets >= 8, "v1 12 kHz up, found by the search: %d packets", v1 ? (int)v1->packets : 0);
    CHECK(rt && rt->packets >= 4, "RTTY 18 kHz down, found by the search: %d packets", rt ? (int)rt->packets : 0);
    CHECK(t.channels.size() >= 3, "%zu channels", t.channels.size());
    if (v1) CHECK(std::fabs(v1->last.offsetHz - 13500) < 300, "v1 offset %.0f Hz", v1->last.offsetHz);
    if (rt) CHECK(std::fabs(rt->last.offsetHz + 16500) < 300 && rt->last.baud == 100 && rt->last.rttyBits == 7, "RTTY offset %.0f Hz, %d baud, %d bits", rt->last.offsetHz, rt->last.baud, rt->last.rttyBits);
    if (v2) {
        CHECK(std::fabs(v2->last.offsetHz - 1500) < 100 && std::fabs(v2->last.toneSpacingHz - 270) < 15, "v2 offset %.0f Hz, spacing %.0f Hz", v2->last.offsetHz, v2->last.toneSpacingHz);
        CHECK(v2->track.size() >= 10 && v2->hasAscent && std::fabs(v2->ascentMs - 5) < 0.6, "track %zu points, climb %.2f m/s", v2->track.size(), v2->ascentMs);
        CHECK(v2->last.custom.size() == 4 && v2->last.custom[0].second == "5.0" && v2->last.custom[1].second == "-12.3", "custom fields %s", v2->last.sentence.c_str());
        CHECK(v2->last.sats == 9 && std::fabs(v2->last.battV - 3.0) < 0.02 && v2->last.speedKmh == 38, "fixed fields");
    }
    for (const auto& p : t.packets) CHECK(p.callsign == "4FSKTEST-V2" || p.callsign == "ID 0" || p.callsign == "ONAIR1", "unexpected packet %s", p.sentence.c_str());

    // a fast radio rate: the shared decimator in front of the channels (5 MS/s -> 2.5 MS/s)
    {
        SynthConfig s5; s5.snrDb = 10; s5.cfoHz = -700; s5.modeOpt[0] = 0; s5.modeOpt[3] = 4;
        auto g5 = makeHorusSynth(s5, 5e6);
        HorusReceiver r5; r5.configure(5e6); r5.setSignalOffset(synthDialHz(5e6));
        std::vector<cf32> b5(50000);
        for (int i = 0; i < 12 * 100; i++) { g5->generate(b5.data(), b5.size()); r5.feed(b5.data(), b5.size()); }
        HorusTelemetry t5; r5.telemetry(t5, 0);
        CHECK(t5.packets.size() >= 2 && std::fabs(t5.packets.front().offsetHz + 700) < 100, "5 MS/s: %zu packets", t5.packets.size());
    }

    // the results file reads back into the same table
    const std::string blob = rx.saveResults();
    HorusReceiver rx2;
    rx2.configure(rate);
    CHECK(rx2.loadResults(blob), "results load");
    HorusTelemetry t2;
    rx2.telemetry(t2, 0);
    CHECK(t2.packets.size() == t.packets.size() && t2.payloads.size() == t.payloads.size(), "results round trip: %zu / %zu packets", t2.packets.size(), t.packets.size());
    CHECK(!rx2.loadResults("horus1\n1\t2\tzz\n"), "a broken results file is refused");

    // the user's tables: a callsign for ID 0 and custom names; a bad JSON is reported and the default kept
    CHECK(rx2.setTables("0, MYBALLOON\n", "").empty(), "tables accepted");
    CHECK(!rx2.setTables("0, MYBALLOON\n", "{\"x\": [").empty(), "bad JSON reported");
    rx2.setTables("0, MYBALLOON\n256, 4FSKTEST-V2\n", "");
    rx2.clearResults();
    CHECK(rx2.loadResults(blob), "reload with the new ID list");
    rx2.telemetry(t2, 0);
    bool named = false;
    for (const auto& p : t2.payloads) named = named || p.callsign == "MYBALLOON";
    CHECK(named || !v1, "ID 0 is shown with the user's callsign");
    return testSummary("test_horus_wide");
}
