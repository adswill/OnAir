// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi frames and PHY pieces (wifi_proto.h): CRC-32 and CRC-16, the scrambled PPDU, beacons built and parsed back for each security.
#include "dect2/wifi/wifi_proto.h"
#include <cstdio>
#include <cstring>
#include <string>
using namespace dect2::wifi;
#include "../common/test_util.h"

int main() {
    const uint8_t s[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(crc32(s, 9) == 0xCBF43926u, "crc32 %08X", crc32(s, 9));
    CHECK(channelMhz(1) == 2412 && channelMhz(6) == 2437 && channelMhz(13) == 2472 && channelMhz(14) == 2484 && channelOf(2437.3) == 6 && channelOf(2440) == 0, "channels");

    // the PPDU descrambles back to SYNC ones, the SFD, a header with a good CRC and the MPDU
    ApSpec a;
    const uint8_t b[6] = {0x00, 0x04, 0x0E, 1, 2, 3};
    memcpy(a.bssid, b, 6);
    a.ssid = "Test net";
    const auto f = makeBeacon(a);
    const auto bits = ppduBits(f, 0x55);
    CHECK(bits.size() == 128 + 16 + 48 + f.size() * 8, "ppdu length %zu", bits.size());
    std::vector<uint8_t> d(bits.size());
    uint8_t r = 0;
    for (size_t i = 0; i < bits.size(); i++) { d[i] = (uint8_t)(bits[i] ^ (r >> 3 & 1) ^ (r >> 6 & 1)); r = (uint8_t)(((r << 1) | bits[i]) & 0x7F); }
    bool ones = true;
    for (int i = 7; i < 128; i++) ones &= d[(size_t)i] == 1;
    uint16_t sfd = 0;
    for (int k = 0; k < 16; k++) sfd |= (uint16_t)(d[(size_t)(128 + k)] << k);
    uint16_t crc = 0;
    for (int k = 0; k < 16; k++) crc = (uint16_t)(crc << 1 | d[(size_t)(176 + k)]);
    CHECK(ones && sfd == kSfd && crc16(&d[144], 32) == crc, "descrambled preamble / header");
    CHECK(crc32(f.data(), f.size() - 4) == ((uint32_t)f[f.size() - 4] | (uint32_t)f[f.size() - 3] << 8 | (uint32_t)f[f.size() - 2] << 16 | (uint32_t)f[f.size() - 1] << 24), "fcs");

    // each security parses back
    for (int sec = Open; sec <= Owe; sec++) {
        for (int ent = 0; ent < 2; ent++) {
            if (ent && (sec == Open || sec == Wep || sec == Owe || sec == Wpa2Wpa3)) continue;
            a.security = sec; a.enterprise = ent;
            const auto g = makeBeacon(a);
            Frame fr;
            CHECK(parseFrame(g.data(), g.size() - 4, fr), "parse %d", sec);
            CHECK(fr.beacon && fr.ssid == "Test net" && !fr.hidden && fr.channel == 6 && fr.interval == 100, "fields %d", sec);
            CHECK(fr.security == sec && fr.enterprise == (bool)ent, "security %d/%d parsed as %s", sec, ent, securityName(fr.security, fr.enterprise));
            CHECK(ratesText(fr.rates) == "1 2 5.5 11 6 9 12 18 24 36 48 54", "rates %s", ratesText(fr.rates).c_str());
            CHECK(macText(fr.bssid) == "00:04:0E:01:02:03" && std::string(vendorOf(fr.bssid)) == "AVM", "bssid");
        }
    }
    a.security = Wpa2; a.enterprise = false;
    // hidden, and the probe response that names it
    a.hidden = true;
    auto h = makeBeacon(a);
    Frame fh;
    CHECK(parseFrame(h.data(), h.size() - 4, fh) && fh.hidden && fh.ssid.empty(), "hidden");
    const uint8_t sta[6] = {2, 0, 0, 0, 0, 1};
    h = makeBeacon(a, true, sta);
    CHECK(parseFrame(h.data(), h.size() - 4, fh) && fh.probeResp && !fh.hidden && fh.ssid == "Test net", "probe response");
    // a data frame is only classified
    const auto df = makeDataFrame(b, sta, 50);
    CHECK(parseFrame(df.data(), df.size() - 4, fh) && fh.type == 2 && !fh.beacon && fh.ssid.empty(), "data frame");
    // truncated elements do not overrun
    auto t = makeBeacon(a);
    for (size_t n = 0; n < t.size(); n++) { Frame x; parseFrame(t.data(), n, x); }
    printf(fails ? "wifi proto: %d FAILED\n" : "wifi proto: all passed\n", fails);
    return fails ? 1 : 0;
}
