// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The 5 and 6 GHz test scene of the Wi-Fi generator (see wifi_gen.h): OFDM beacons of access points of every generation on one primary channel.
#include "dect2/wifi/wifi_gen.h"
#include "dect2/wifi/wifi_chan.h"
#include "dect2/wifi/wifi_proto.h"
#include <algorithm>
#include <cstring>

namespace dect2 {

// APs on the 20 MHz primary `channel` of band 1 (5 GHz) or 2 (6 GHz): see the header for what each sends.
std::vector<WifiGenFrame> wifiTestSchedule56(int band, int channel) {
    using namespace wifi;
    std::vector<WifiGenFrame> v;
    auto add = [&](std::vector<uint8_t> f, double gap, double cfo, double amp) {
        WifiGenFrame g;
        g.mpdu = std::move(f); g.gapSec = gap; g.cfoHz = cfo; g.ampScale = amp; g.phy = 1;
        v.push_back(std::move(g));
    };
    auto mac = [](ApSpec& a, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4, uint8_t b5) { const uint8_t m[6] = {b0, b1, b2, b3, b4, b5}; memcpy(a.bssid, m, 6); };
    const int wide = std::max(20, std::min(80, band == Band6 ? 80 : 80));
    ApSpec a;   // a modern AP: HE (and EHT on 6 GHz), 80 MHz, WPA3 / OWE on 6 GHz
    mac(a, 0x24, 0xA4, 0x3C, 0x50, 0x00, 0x01);
    a.band = band; a.channel = channel; a.ssid = "OnAir-5G"; a.security = Wpa2Wpa3; a.he = true; a.ht = band == Band5; a.vht = band == Band5; a.eht = band == Band6;
    a.width = blockCentreChannel(band, channel, wide) ? wide : 20; a.streams = 2; a.bssColor = 17; a.twt = true; a.country = "DE I"; a.powerConstraint = 3;
    a.tsf = 5000000; a.seq = 300; a.mdid = 0x1234; a.he6Reg = 0;
    if (band == Band6) { a.ssid = "OnAir-6E"; a.security = Wpa3; a.he6Dup = true; mac(a, 0x24, 0xA4, 0x3C, 0x60, 0x00, 0x01); }
    ApSpec b;   // an older AP: 802.11n only (5 GHz), or an HE AP with OWE (6 GHz)
    mac(b, 0x00, 0x04, 0x0E, 0x77, 0x88, 0x99);
    b.band = band; b.channel = channel; b.ssid = band == Band5 ? "OnAir-Old" : "OnAir-6E-Open"; b.security = band == Band5 ? Wpa2 : Owe; b.ht = band == Band5; b.he = band == Band6;
    b.width = 20; b.streams = band == Band5 ? 3 : 1; b.tsf = 777; b.seq = 40; b.interval = 100; b.bssColor = band == Band6 ? 33 : -1;
    ApSpec c;   // a Wi-Fi 5 AP (5 GHz) or a hidden HE AP (6 GHz)
    mac(c, 0xFC, 0xEC, 0xDA, 0x12, 0x34, 0x56);
    c.band = band; c.channel = channel; c.ssid = band == Band5 ? "OnAir-ac" : "hidden6"; c.hidden = band == Band6; c.security = band == Band5 ? Wpa2 : Wpa3;
    c.ht = band == Band5; c.vht = band == Band5; c.he = band == Band6; c.width = blockCentreChannel(band, channel, 40) ? 40 : 20; c.streams = 4; c.tsf = 31337; c.seq = 9;
    if (band == Band5) {   // a co-located 6 GHz AP announced in the Reduced Neighbor Report
        Neighbor n;
        n.opClass = 131; n.channel = 5; n.hasBssid = true; n.hasShortSsid = true; n.coLocated = true; n.unsolicitedProbe = true; n.sameSsid = true; n.memberOfEss24or5 = false;
        const uint8_t m[6] = {0x24, 0xA4, 0x3C, 0x60, 0x00, 0x01};
        memcpy(n.bssid, m, 6);
        n.shortSsid = shortSsid("OnAir-6E");
        a.rnr.push_back(n);
    }
    add(makeBeacon(a), 0.0008, 0, 1.0);
    add(makeBeacon(b), 0.0008, 15e3, 0.7);
    add(makeBeacon(c), 0.0008, -20e3, 0.85);
    const uint8_t sta[6] = {0x3A, 0x01, 0x02, 0x03, 0x04, 0x05};
    add(makeBeacon(a, true, sta), 0.0008, 0, 1.0);          // a probe response to a client
    if (band == Band6) {
        add(makeFilsDiscovery(a), 0.0008, 0, 1.0);          // FILS discovery
        add(makeBeacon(a, true, nullptr), 0.0008, 0, 1.0);  // an unsolicited probe response (broadcast address)
    }
    return v;
}

} // namespace dect2
