// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// WSPRnet: form encoding, dedupe, and the sender staying off until configured.
#include "dect2/net/wsprnet.h"
#include <cstdio>
#include <cstring>
using namespace dect2;
using namespace dect2::wsprnet;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    CHECK(urlEncodeForm("KO24ab") == "KO24ab" && urlEncodeForm("a b") == "a%20b" && urlEncodeForm("a&b=c") == "a%26b%3Dc", "urlEncode");
    SpotFields s;
    s.rcall = "LY1ABC"; s.rgrid = "KO24ab"; s.rqrgMhz = 14.095600;
    s.tcall = "DL1XYZ"; s.tgrid = "JO62"; s.tqrgMhz = 14.097090;
    s.sigDb = -12; s.dt = 0.3; s.drift = 0; s.dbm = 37; s.slotUtc = 1700000000;   // 2023-11-14 22:13:20 UTC
    const std::string body = encodeSpotBody(s, "OnAir-0.2.6");
    CHECK(body.find("function=wspr") != std::string::npos, "function");
    CHECK(body.find("rcall=LY1ABC") != std::string::npos && body.find("rgrid=KO24ab") != std::string::npos, "receiver");
    CHECK(body.find("rqrg=14.095600") != std::string::npos && body.find("tqrg=14.097090") != std::string::npos, "freq");
    CHECK(body.find("tcall=DL1XYZ") != std::string::npos && body.find("tgrid=JO62") != std::string::npos, "tx");
    CHECK(body.find("sig=-12") != std::string::npos && body.find("dt=0.3") != std::string::npos && body.find("drift=0") != std::string::npos, "metrics");
    CHECK(body.find("dbm=37") != std::string::npos && body.find("mode=2") != std::string::npos, "dbm mode");
    CHECK(body.find("version=OnAir-0.2.6") != std::string::npos, "version");
    CHECK(body.find("date=231114") != std::string::npos && body.find("time=2213") != std::string::npos, "slot time: %s", body.c_str());
    Dedupe d(130);
    CHECK(d.accept("DL1XYZ", 1700000000, 1409709, 1000), "first");
    CHECK(!d.accept("DL1XYZ", 1700000000, 1409709, 1010), "dup same slot");
    CHECK(d.accept("DL1XYZ", 1700000120, 1409709, 1010), "next slot");
    CHECK(d.accept("DL1XYZ", 1700000000, 1409710, 1010), "other tone");
    WsprNet::get().setDialHz(14095600);
    WsprNet::get().spot(1500, -5, 0.2f, "DL1XYZ", "JO62", 30, 1700000000);
    CHECK(!WsprNet::get().want() && WsprNet::get().status().state == "off", "off by default");
    WsprNetConfig c; c.on = true; c.call = "ly1abc"; c.locator = "KO24ab";
    WsprNet::get().configure(c);
    CHECK(WsprNet::get().want(), "on");
    WsprNet::get().spot(1500, -5, 0.2f, "DL1XYZ", "JO62", 30, 1700000000);
    WsprNet::get().spot(1500, -5, 0.2f, "DL1XYZ", "JO62", 30, 1700000000);
    CHECK(WsprNet::get().status().duplicates == 1 && WsprNet::get().status().state.find("1 spots waiting") != std::string::npos, "queued: %s", WsprNet::get().status().state.c_str());
    c.call = "x"; WsprNet::get().configure(c);
    CHECK(!WsprNet::get().want(), "invalid callsign");
    WsprNet::get().stop();
    printf(fails ? "FAILED %d\n" : "wsprnet OK\n", fails);
    return fails != 0;
}
