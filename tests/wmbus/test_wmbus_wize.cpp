// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wize (169 MHz, Wize Alliance LAN protocol v1.x on EN 13757-4 mode N) in the Wireless M-Bus receiver.
//   1. frame level: the PRES-EXCHANGE fields round-trip through a format B frame; a damaged byte fails the CRC; a frame cut short is refused
//   2. over the air: the 169 MHz plan with two Wize meters (2.4 kbit/s with the L7 encrypted, 4.8 kbit/s with key selector 0) and the plain N1 meter,
//      clean, with transmitter frequency errors of -1.5 / +1.2 kHz, at 20 dB; every frame decoded is right
// All signals are simulated: no Wize recording was available.
#include "dect2/wmbus/wmbus_gen.h"
#include "dect2/wmbus/wmbus_live.h"
#include "dect2/wmbus/wmbus_proto.h"
#include "dect2/wmbus/wmbus_tel.h"
#include <cstdio>
#include <set>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::wmbus;
#include "../common/test_util.h"

static WmbusTelemetry run(const WmbusGenConfig& c, double seconds) {
    WmbusGenerator g(c);
    WmbusLiveReceiver rx;
    rx.configure(c.rate);
    rx.setCenterHz(c.centreHz);
    std::vector<cf32> b(20000);
    for (size_t done = 0; done < (size_t)(seconds * c.rate); done += b.size()) { g.generate(b.data(), b.size()); rx.feed(b.data(), b.size()); }
    WmbusTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

int main() {
    {   // 1. frame level
        const uint8_t id4[4] = {0x87, 0x65, 0x43, 0x21};
        std::vector<uint8_t> l7 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        const auto air = buildFrame(wizeBody(mfrStringToCode("SAG"), id4, 1, 3, 0x44, 3, 0x0A, 0x1234, 0xFE, l7, 0xDEADBEEF, 0x4321, 0xBEEF), true);
        Frame f;
        CHECK(parseFrame(air.data(), air.size(), true, f) && f.crcOk, "Wize frame did not parse: %s", f.error.c_str());
        CHECK(f.wize && f.ci == 0x20 && f.wizeVers == 1 && f.wizeKeySel == 3 && f.wizeNetwId == 0x0A && f.wizeCpt == 0x1234 && f.wizeApp == 0xFE,
              "L6 fields: vers %d keysel %d netw %d cpt %d app %d", f.wizeVers, f.wizeKeySel, f.wizeNetwId, f.wizeCpt, f.wizeApp);
        CHECK(f.wizeHashKenc == 0xDEADBEEF && f.wizeTStamp == 0x4321 && f.wizeHashKmac == 0xBEEF && f.wizeL7 == l7, "L6 trailer fields");
        CHECK(f.encrypted && f.mfr == "SAG" && f.id == "21436587", "encrypted %d mfr %s id %s", (int)f.encrypted, f.mfr.c_str(), f.id.c_str());
        for (size_t i = 1; i < air.size(); i += 5) {
            auto bad = air;
            bad[i] ^= 0x10;
            Frame g;
            CHECK(!parseFrame(bad.data(), bad.size(), true, g) || !g.crcOk, "a flipped bit at byte %zu passed", i);
        }
        const auto shortAir = buildFrame(std::vector<uint8_t>{0x44, 0x01, 0x02, 1, 2, 3, 4, 1, 3, 0x20, 0x21, 1}, true);
        Frame s;
        CHECK(!parseFrame(shortAir.data(), shortAir.size(), true, s), "a cut-off Wize frame was taken");
        Frame k0;
        const auto clear = buildFrame(wizeBody(mfrStringToCode("ITW"), id4, 1, 7, 0x44, 0, 0x0A, 7, 0xFE, l7, 1, 2, 3), true);
        CHECK(parseFrame(clear.data(), clear.size(), true, k0) && !k0.encrypted && k0.payload == l7, "key selector 0: L7 in clear");
    }
    // 2. over the air
    struct Case { const char* name; double snr, cfo; };
    for (const Case& cs : {Case{"clean", 40, 0}, Case{"-1.5 kHz", 30, -1500}, Case{"+1.2 kHz", 30, 1200}, Case{"20 dB", 20, 0}}) {
        WmbusGenConfig c; c.plan = 1; c.centreHz = 169.4375e6; c.snrDb = cs.snr; c.cfoHz = cs.cfo; c.gapSec = 0.05;
        const WmbusTelemetry t = run(c, 12);
        int wize24 = 0, wize48 = 0, n1 = 0, wrong = 0;
        for (const auto& l : t.log) {
            if (!l.crcOk) continue;
            if (l.id == "21436587" && l.mfr == "SAG" && l.phy == (int)Phy::N24 && l.summary.find("Wize v1 net 10 app 254") == 0 && l.summary.find("L7 encrypted") != std::string::npos) wize24++;
            else if (l.id == "13572468" && l.mfr == "ITW" && l.phy == (int)Phy::N1 && l.summary.find("Wize v1 net 10 app 254") == 0 && l.summary.find("L7 in clear") != std::string::npos) wize48++;
            else if (l.id == "94005566" && l.mfr == "ITW" && l.phy == (int)Phy::N1) n1++;
            else { wrong++; printf("    WRONG: %s %s phy %d %s\n", l.mfr.c_str(), l.id.c_str(), l.phy, l.summary.c_str()); }
        }
        printf("  %-10s Wize 2.4k %d, Wize 4.8k %d, N1 %d, wrong %d, CRC failures %llu\n", cs.name, wize24, wize48, n1, wrong, (unsigned long long)t.blocksBad);
        CHECK(wrong == 0, "%s: %d wrong frames", cs.name, wrong);
        if (cs.snr >= 30) CHECK(wize24 >= 3 && wize48 >= 3 && n1 >= 3, "%s: Wize 2.4k %d, 4.8k %d, N1 %d", cs.name, wize24, wize48, n1);
        else CHECK(wize24 + wize48 >= 2, "%s: Wize frames %d", cs.name, wize24 + wize48);
    }
    return testSummary("wmbus wize");
}
