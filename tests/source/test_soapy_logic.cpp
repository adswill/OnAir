// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pure SoapySDR helper logic against fake driver tables (no hardware).
#include "../../core/src/source/soapy_logic.h"
#include <cstdio>
#include <cstdlib>
using namespace dect2::soapylogic;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
    CHECK(clampToSpan(50, {0, 40, 0}) == 40);
    CHECK(clampToSpan(-3, {0, 40, 0}) == 0);
    CHECK(clampToSpan(13, {0, 40, 8}) == 16);
    CHECK(clampToSpan(39, {0, 40, 8}) == 40);
    CHECK(clampToSpan(41, {0, 38, 8}) == 32);
    // Airspy-like fixed rates
    std::vector<Span> air = {{2.5e6, 2.5e6, 0}, {10e6, 10e6, 0}};
    auto r = chooseRate(air, 4e6); CHECK(r.hz == 10e6 && !r.exact && !r.note.empty());
    r = chooseRate(air, 2.5e6); CHECK(r.exact && r.note.empty());
    r = chooseRate(air, 20e6); CHECK(r.hz == 10e6 && r.note.find("only offers") != std::string::npos);
    // RTL-like continuous ranges
    std::vector<Span> rtl = {{225001, 300000, 0}, {900001, 3200000, 0}};
    r = chooseRate(rtl, 2.4e6); CHECK(r.exact);
    r = chooseRate(rtl, 500000); CHECK(r.hz == 900001 && !r.note.empty());
    CHECK(chooseRate({}, 1e6).hz == 1e6);
    CHECK(chooseBandwidth({{1.75e6, 1.75e6, 0}, {5e6, 5e6, 0}}, 4e6) == 5e6);
    CHECK(stageName("LNA").label == "Low-noise amplifier (LNA)");
    CHECK(stageName("weird").label == "Gain stage weird" && stageName("x").unit == "dB");
    CHECK(stageMemoryKey("S1", "fm", "VGA") == "soapy/S1/fm/gain.VGA");
    CHECK(stageMemoryKey("", "fm", "VGA") != stageMemoryKey("S1", "fm", "VGA"));
    const auto a = parseArgs("driver=remote,remote=tcp://h:55132,remote:driver=rtlsdr,serial=00001");
    CHECK(a.at("remote") == "tcp://h:55132" && a.at("serial") == "00001");
    CHECK(isRemote("driver=remote,remote:driver=rtlsdr") && !isRemote("driver=rtlsdr"));
    CHECK(effectiveDriver("driver=remote,remote:driver=rtlsdr") == "rtlsdr" && effectiveDriver("driver=lime") == "lime");
    CHECK(selectBySerial("rtlsdr", "123") == "driver=rtlsdr,serial=123" && selectBySerial("rtlsdr", "").empty());
    CHECK(selectBySerial("rtlsdr", "9", "tcp://h") == "driver=remote,remote=tcp://h,remote:driver=rtlsdr,serial=9");
    const auto u = uniqueLabels({"RTL", "Lime", "RTL", "RTL"});
    CHECK(u[0] == "RTL" && u[2] == "RTL (2)" && u[3] == "RTL (3)");
    const auto h = diffKeys({"a", "b"}, {"b", "c"});
    CHECK(h.added.size() == 1 && h.added[0] == "c" && h.removed.size() == 1 && h.removed[0] == "a");
    CHECK(streamStatus(0, 0, 0).empty());
    CHECK(streamStatus(1, 2, 3) == "1 overflow (host too slow), 2 underflows, 3 reconnects");
    CHECK(!busyHint("usb_claim_interface error -6").empty() && busyHint("timeout").empty());
    printf(fails ? "FAILED\n" : "soapy_logic OK\n");
    return fails ? 1 : 0;
}
