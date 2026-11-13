// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRaWAN user-key decode against the lora-packet example frame, Class B beacons against the LoRaWAN 1.0.2 spec's examples, and a beacon through the LoRa receiver bank with noise. Simulated RF only.
#include "dect2/lorawan/lora_bank.h"
#include "dect2/lorawan/lorawan_crypto.h"
#include "dect2/lorawan/lorawan_gen.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::lorawan;
#include "../common/test_util.h"
#undef CHECK
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static std::vector<uint8_t> hx(const char* s) { std::vector<uint8_t> v; for (size_t i = 0; s[i] && s[i + 1]; i += 2) { char b[3] = {s[i], s[i + 1], 0}; v.push_back((uint8_t)strtol(b, nullptr, 16)); } return v; }

static void beaconSpec() {
    // spec 15.2 example: NetID CCBBAA, Time CC020000 (0xCC020000 sent as 00 00 02 CC), CRC-8 7E, InfoDesc 0, lat 002001, long 038100, CRC-16 55DE
    Beacon b = parseBeacon(hx("AABBCC000002CC7E00012000008103DE55").data(), 17);
    CHECK(b.ok && !b.newFormat && b.netId == 0xCCBBAA && b.timeSec == 0xCC020000 && b.hasGps && b.infoDesc == 0);
    CHECK(std::fabs(b.lat - 0x002001 * 90.0 / 8388608.0) < 1e-9 && std::fabs(b.lon - 0x038100 * 180.0 / 8388608.0) < 1e-9);
    auto bad = hx("AABBCC000002CC7E00012000008103DE55"); bad[9] ^= 1;
    CHECK(!parseBeacon(bad.data(), 17).ok);
    CHECK(beaconCrc(hx("AABBCC000002CC").data(), 7) == 0xC87E);
    // 1.0.3 style: RFU 2, Time 4, CRC-16, gateway, CRC-16, built from the same CRC
    std::vector<uint8_t> nw = {0, 0, 0x00, 0x00, 0x02, 0xCC};
    uint16_t c = beaconCrc(nw.data(), 6);
    nw.push_back((uint8_t)c); nw.push_back((uint8_t)(c >> 8));
    std::vector<uint8_t> gw = {1, 0xCD, 0xCC, 0x2C, 0x34, 0x12, 0x00};
    uint16_t c2 = beaconCrc(gw.data(), 7);
    nw.insert(nw.end(), gw.begin(), gw.end()); nw.push_back((uint8_t)c2); nw.push_back((uint8_t)(c2 >> 8));
    b = parseBeacon(nw.data(), 17);
    CHECK(b.ok && b.newFormat && b.infoDesc == 1 && b.timeSec == 0xCC020000);
}

static void userKeys() {
    // lora-packet README example
    const auto p = hx("40F17DBE4900020001954378762B11FF0D");
    const auto nwk = hx("44024241ed4ce9a68c6a8bc055233fd3"), app = hx("ec925802ae430ca77fd3dd73cb2cc588");
    const uint32_t a = 0x49BE7DF1;
    CHECK(p.size() >= 13);
    uint32_t mic = (uint32_t)p[p.size() - 4] | (uint32_t)p[p.size() - 3] << 8 | (uint32_t)p[p.size() - 2] << 16 | (uint32_t)p[p.size() - 1] << 24;
    CHECK(dataMic(p.data(), p.size() - 4, nwk.data(), true, a, 2) == mic);
    uint8_t wrong[16] = {1};
    CHECK(dataMic(p.data(), p.size() - 4, wrong, true, a, 2) != mic);
    const auto pl = decryptFrm(p.data() + 9, p.size() - 9 - 4, app.data(), true, a, 2);
    CHECK(std::string((const char*)pl.data(), pl.size()) == "test");
}

static void beaconRf() {
    auto frame = hx("AABBCC000002CC7E00012000008103DE55");
    for (double snr : {25.0, 5.0}) for (double off : {0.0, 3000.0}) {
        LorawanGenConfig g; g.rate = 1e6; g.dialMhz = 869.525; g.snrDb = snr; g.cfoHz = off; g.loop = false; g.leadSec = 0.05;
        LorawanGenFrame f; f.mhz = 869.525; f.payload = frame; f.gapSec = 0.1;
        f.p.sf = 9; f.p.bwHz = 125e3; f.p.cr = 5; f.p.preamble = 10; f.p.syncWord = 0x34; f.p.implicitHdr = true; f.p.implicitLen = 17; f.p.crc = false; f.p.ldro = false;
        g.frames.push_back(f);
        LorawanGenerator gen(g);
        lorabank::Bank bank;
        lorabank::Channel c; c.offsetHz = 0; c.bwHz = 125e3; c.syncs = {0x34}; c.sfs = {9}; c.implicitLen = 17; c.preamble = 10;
        bank.configure(1e6, {c});
        std::vector<cf32> buf(4096);
        std::vector<lorabank::Decoded> got;
        for (int i = 0; i < 300; i++) { gen.generate(buf.data(), buf.size()); bank.feed(buf.data(), buf.size(), got); }
        int ok = 0;
        for (const auto& d : got) { const Beacon b = parseBeacon(d.f.payload.data(), d.f.payload.size()); if (b.ok && b.timeSec == 0xCC020000) ok++; }
        printf("beacon snr %.0f offs %.0f: decoded %d\n", snr, off, ok);
        CHECK(ok == 1);
    }
}

int main() { beaconSpec(); userKeys(); beaconRf(); return testSummary("lorawan_crypto"); }
