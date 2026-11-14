// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// PlutoSDR antenna port settings: only the exact "rx2" / "tx2" pick the second port, anything else keeps RX1 / TX1 (today's behaviour).
#include "dect2/source/pluto_ports.h"
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    std::map<std::string, std::string> r;
    CHECK(plutoRxPort(r) == 0 && plutoTxPort(r, 0) == 0, "empty settings: RX1 and TX1");
    r["rxport"] = "rx2"; r["txport"] = "tx2";
    CHECK(plutoRxPort(r) == 1 && plutoTxPort(r, 0) == 1, "rx2 / tx2");
    r["rxport"] = "rx1"; r["txport"] = "tx1";
    CHECK(plutoRxPort(r) == 0 && plutoTxPort(r, 0) == 0, "rx1 / tx1");
    r["rxport"] = "RX2 "; r["txport"] = "";
    CHECK(plutoRxPort(r) == 0 && plutoTxPort(r, 0) == 0, "garbled values keep the first port");
    r["txport"] = "tx1";
    CHECK(plutoTxPort(r, 1) == 1, "an explicit channel wins");
    r["rxport"] = "tx2";
    CHECK(plutoRxPort(r) == 0, "a value of the other direction is not accepted");
    return testSummary("pluto_ports");
}
