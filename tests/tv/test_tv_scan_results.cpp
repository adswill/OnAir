// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TV channel scan results persist in app-<tv-scan>.bin (results_io.h); round-trip a representative row.
#include "dect2/results/results_io.h"
#include "dect2/engine/scanner.h"
#include <cstdio>
#include <vector>

using namespace dect2;

int main() {
    int fail = 0;
    auto check = [&](bool ok, const char* msg) {
        if (!ok) { fprintf(stderr, "tv scan results: %s\n", msg); fail++; }
    };
    std::vector<ScanResult> in(1);
    ScanResult& r = in[0];
    r.freqMHz = 474.0;
    r.bwMhz = 8;
    r.t2 = r.seen = true;
    r.standard = "DVB-T2";
    r.mode = "T2-Base";
    r.snrDb = 21.5f;
    r.networkName = "TestNet";
    r.services = {"One TV", "Two Radio"};
    const std::string blob = results::pack(in);
    std::vector<ScanResult> out;
    check(results::unpack(blob, out) && out.size() == 1 && out[0].networkName == "TestNet" && out[0].services.size() == 2, "round trip");
    std::vector<int> bad;
    check(!results::unpack(blob, bad), "wrong type refused");
    printf(fail ? "tv scan results: %d FAILED\n" : "tv scan results: all passed\n", fail);
    return fail ? 1 : 0;
}
