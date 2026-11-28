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
    // both inputs at once (#398): off unless exactly on; the interleaved frames split into the two streams
    std::map<std::string, std::string> d;
    CHECK(!plutoDualRx(d), "dual RX is off by default");
    d["dualrx"] = "1"; CHECK(plutoDualRx(d), "dualrx 1 turns it on");
    d["dualrx"] = "0"; CHECK(!plutoDualRx(d), "dualrx 0 is off");
    d["dualrx"] = "yes"; CHECK(!plutoDualRx(d), "a garbled value is off");
    const int16_t fr[8] = {2048, -2048, 1024, 0, -1024, 512, 0, 2048};
    std::complex<float> a[2], b[2];
    CHECK(plutoSplitDual(fr, 2, a, b) == 2, "two frames split");
    CHECK(a[0] == std::complex<float>(1.f, -1.f) && b[0] == std::complex<float>(0.5f, 0.f), "frame 0: I1 Q1 to the first stream, I2 Q2 to the second");
    CHECK(a[1] == std::complex<float>(-0.5f, 0.25f) && b[1] == std::complex<float>(0.f, 1.f), "frame 1");
    CHECK(plutoIsRx2Uri("usb:1.2.3#rx2") && !plutoIsRx2Uri("usb:1.2.3") && plutoBaseUri("ip:a#rx2") == "ip:a" && plutoBaseUri("ip:a") == "ip:a", "the RX2 entry's address");
    return testSummary("pluto_ports");
}
