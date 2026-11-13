// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RBDS: the US programme type table, call letters from the PI code (round trips over the whole K / W range and the known edges), the
// Auto choice from the tuned frequency, and the receiver end to end: one station sending PI of WXYZ and PTY 10, read as RDS (Pop music) and as RBDS (Country).
#include "dect2/fm/fm_gen.h"
#include "dect2/fm/fm_rbds.h"
#include "dect2/fm/fm_rx.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static FmTelemetry run(int region, double mhz, int pi, int pty, double secs = 8) {
    FmGenConfig cfg; cfg.rate = 2e6; cfg.cnrDb = 40; cfg.pi = pi; cfg.pty = pty;
    FmGenerator gen(cfg);
    FmReceiver rx;
    rx.setSilent(true);
    rx.configure(cfg.rate);
    rx.setRdsRegion(region, mhz);
    std::vector<cf32> buf; FmTelemetry t; uint64_t seq = 0;
    const size_t total = (size_t)(secs * cfg.rate);
    for (size_t done = 0; done < total; done += 65536) {
        buf.clear(); gen.generate(std::min<size_t>(65536, total - done), buf);
        rx.feed(buf.data(), buf.size());
        FmTelemetry n; if (rx.telemetry(n, seq)) { seq = n.seq; t = n; }
    }
    return t;
}

int main() {
    // the table: the 32 RBDS names in order (NRSC-4-B), different from the European ones, the same where the standards agree
    const char* const rbds[32] = {"None", "News", "Information", "Sports", "Talk", "Rock", "Classic rock", "Adult hits", "Soft rock", "Top 40", "Country",
        "Oldies", "Soft", "Nostalgia", "Jazz", "Classical", "Rhythm and blues", "Soft R&B", "Foreign language", "Religious music", "Religious talk",
        "Personality", "Public", "College", "Spanish talk", "Spanish music", "Hip hop", "Unassigned", "Unassigned", "Weather", "Emergency test", "Emergency"};
    for (int i = 0; i < 32; i++) CHECK(std::strcmp(rdsPtyName(i, true), rbds[i]) == 0, "RBDS PTY %d is '%s'", i, rdsPtyName(i, true));
    CHECK(std::strcmp(rdsPtyName(10, false), "Pop music") == 0 && std::strcmp(rdsPtyName(10, true), "Country") == 0, "PTY 10 differs");
    CHECK(std::strcmp(rdsPtyName(33, true), "News") == 0, "PTY wraps to 5 bits");
    // call letters: the edges of the range, then every sign both ways
    CHECK(rbdsCallSign(0x1000) == "KAAA" && rbdsCallSign(0x54A7) == "KZZZ", "K range %s %s", rbdsCallSign(0x1000).c_str(), rbdsCallSign(0x54A7).c_str());
    CHECK(rbdsCallSign(0x54A8) == "WAAA" && rbdsCallSign(0x994F) == "WZZZ", "W range %s %s", rbdsCallSign(0x54A8).c_str(), rbdsCallSign(0x994F).c_str());
    CHECK(rbdsCallSign(0x0FFF).empty() && rbdsCallSign(0x9950).empty() && rbdsCallSign(0).empty() && rbdsCallSign(0xD3A1).empty(), "outside the range");
    CHECK(rbdsCallSign(0x1001) == "KAAB" && rbdsCallSign(0x101A) == "KABA" && rbdsCallSign(0x1000 + 676) == "KBAA", "digit order");
    int bad = 0;
    for (int pi = 0x1000; pi <= 0x994F; pi++) { const std::string c = rbdsCallSign(pi); if (c.size() != 4 || rbdsPiFromCallSign(c) != pi) bad++; }
    CHECK(bad == 0, "%d PI codes do not round trip", bad);
    for (const char* c : {"KAAA", "WZZZ", "WXYZ", "KQED", "wabc", "KROQ"}) {
        std::string up = c; for (auto& ch : up) ch = (char)std::toupper((unsigned char)ch);
        const int pi = rbdsPiFromCallSign(c);
        CHECK(pi >= 0x1000 && pi <= 0x994F && rbdsCallSign(pi) == up, "%s -> %04X -> %s", c, pi, rbdsCallSign(pi).c_str());
    }
    CHECK(rbdsPiFromCallSign("XABC") == 0 && rbdsPiFromCallSign("WAB") == 0 && rbdsPiFromCallSign("W1BC") == 0 && rbdsPiFromCallSign("") == 0, "bad call signs");
    // Auto: odd tenths are RBDS, even tenths RDS, the explicit choices win
    CHECK(rdsUseRbds(kRdsAuto, 87.9) && rdsUseRbds(kRdsAuto, 101.1) && rdsUseRbds(kRdsAuto, 107.9) && rdsUseRbds(kRdsAuto, 93.9), "odd tenths");
    CHECK(!rdsUseRbds(kRdsAuto, 88.0) && !rdsUseRbds(kRdsAuto, 100.0) && !rdsUseRbds(kRdsAuto, 98.2) && !rdsUseRbds(kRdsAuto, 98.25), "even tenths");
    CHECK(rdsUseRbds(kRdsUs, 100.0) && !rdsUseRbds(kRdsEurope, 101.1), "explicit choices");
    // the receiver: PI of WXYZ, PTY 10, tuned to 101.1 MHz
    const int pi = rbdsPiFromCallSign("WXYZ");
    {
        const FmTelemetry t = run(kRdsAuto, 101.1, pi, 10);
        printf("auto 101.1: PI %04X, '%s' call '%s' rbds %d\n", t.piCode, t.ptyText.c_str(), t.callSign.c_str(), (int)t.rbds);
        CHECK(t.piCode == pi && t.rbds && t.ptyText == "Country" && t.callSign == "WXYZ", "auto at an odd tenth: PI %04X '%s' '%s'", t.piCode, t.ptyText.c_str(), t.callSign.c_str());
    }
    {
        const FmTelemetry t = run(kRdsAuto, 100.0, pi, 10);
        CHECK(!t.rbds && t.ptyText == "Pop music" && t.callSign.empty(), "auto at an even tenth: '%s' '%s'", t.ptyText.c_str(), t.callSign.c_str());
    }
    {
        const FmTelemetry t = run(kRdsEurope, 101.1, pi, 10);
        CHECK(!t.rbds && t.ptyText == "Pop music" && t.callSign.empty() && t.piCode == pi, "Europe forced: '%s'", t.ptyText.c_str());
    }
    {
        const FmTelemetry t = run(kRdsUs, 100.0, pi, 10);
        CHECK(t.rbds && t.ptyText == "Country" && t.callSign == "WXYZ", "US forced: '%s' '%s'", t.ptyText.c_str(), t.callSign.c_str());
    }
    {   // a PI outside the call-letter range (a European code) in RBDS: names switch, no call letters invented
        const FmTelemetry t = run(kRdsUs, 101.1, 0xD318, 1);
        CHECK(t.rbds && t.ptyText == "News" && t.callSign.empty(), "non-US PI: '%s' '%s'", t.ptyText.c_str(), t.callSign.c_str());
    }
    return testSummary("fm_rbds");
}
