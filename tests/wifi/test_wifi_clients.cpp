// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi client view: probe requests, data frames with their network, deauth flood alarm, EAPOL counted, addresses hashed. Simulated frames only.
#include "dect2/wifi/wifi_clients.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"
#undef CHECK
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static std::vector<uint8_t> frame(int type, int sub, int fc1, const uint8_t* a1, const uint8_t* a2, const uint8_t* a3, std::vector<uint8_t> body) {
    std::vector<uint8_t> f(24);
    f[0] = (uint8_t)((type << 2) | (sub << 4)); f[1] = (uint8_t)fc1;
    std::memcpy(&f[4], a1, 6); std::memcpy(&f[10], a2, 6); std::memcpy(&f[16], a3, 6);
    f.insert(f.end(), body.begin(), body.end());
    return f;
}

int main() {
    const uint8_t sta[6] = {0x02, 1, 2, 3, 4, 5}, ap[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60}, bc[6] = {255, 255, 255, 255, 255, 255};
    WifiClientTracker t(12345);
    // probe for an SSID and a wildcard probe
    std::vector<uint8_t> ie = {0, 6, 'M', 'y', 'H', 'o', 'm', 'e', 1, 1, 0x82};
    auto f = frame(0, 4, 0, bc, sta, bc, ie);
    t.observe(f.data(), f.size(), "", 100, 1.0);
    f = frame(0, 4, 0, bc, sta, bc, {0, 0});
    t.observe(f.data(), f.size(), "", 101, 1.1);
    CHECK(t.clients().size() == 1 && t.clients()[0].probeFrames == 2 && t.clients()[0].probes.size() == 1 && t.clients()[0].probes[0] == "MyHome");
    const std::string tag = t.clients()[0].tag;
    CHECK(tag.find("02:") == std::string::npos && tag.size() == 8);
    WifiClientTracker other(99); CHECK(other.tagOf(sta) != t.tagOf(sta));
    // data to the AP, QoS with the EAPOL LLC: counted once, tagged to the AP
    std::vector<uint8_t> eapol = {0xAA, 0xAA, 3, 0, 0, 0, 0x88, 0x8E, 2, 3, 0, 0x5F};
    f = frame(2, 0, 1, ap, sta, ap, eapol);
    t.observe(f.data(), f.size(), "10:20:30:40:50:60", 110, 2.0);
    CHECK(t.handshakes() == 1 && t.clients()[0].withAp == "10:20:30:40:50:60" && t.clients()[0].dataFrames == 1);
    // a protected frame, even with those bytes, is not read: it would be ciphertext
    f = frame(2, 0, 1 | 0x40, ap, sta, ap, eapol);
    t.observe(f.data(), f.size(), "10:20:30:40:50:60", 110, 2.1);
    CHECK(t.handshakes() == 1);
    // deauth flood: 14 frames, no alarm; 15th alarms once
    auto d = frame(0, 12, 0, sta, ap, ap, {7, 0});
    for (int i = 0; i < 14; i++) t.observe(d.data(), d.size(), "10:20:30:40:50:60", 200, 10.0 + i * 0.1);
    CHECK(t.alarms().empty());
    t.observe(d.data(), d.size(), "10:20:30:40:50:60", 201, 11.5);
    CHECK(t.alarms().size() == 1 && t.alarms()[0].bssid == "10:20:30:40:50:60" && t.alarms()[0].frames == 15);
    for (int i = 0; i < 20; i++) t.observe(d.data(), d.size(), "10:20:30:40:50:60", 202, 12.0 + i * 0.1);
    CHECK(t.alarms().size() == 1);   // one alarm a minute
    CHECK(t.deauths() == 35);
    // garbage
    uint8_t g[10] = {}; t.observe(g, 10, "", 300, 20.0);
    return testSummary("wifi_clients");
}
