// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi client view: stations probing for networks, stations seen with an access point, deauthentication floods and counts of 4-way handshakes.
// Station addresses are hashed with a random per-run salt and shown as a short tag: they are never stored or shown as MAC addresses. Handshakes are
// counted only, never captured.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct WifiClientInfo {
    std::string tag;            // short hash of the address (different every run)
    std::string withAp;         // access point it talks to, "" when only probing
    std::vector<std::string> probes;   // SSIDs it asked for (up to 12, most recent last)
    uint64_t probeFrames = 0, dataFrames = 0;
    int64_t lastWall = 0;
};

struct WifiFloodAlarm { int64_t wallTime = 0; std::string bssid; int frames = 0; };

class WifiClientTracker {
public:
    explicit WifiClientTracker(uint32_t salt) : salt_(salt) {}
    // p, n: a complete 802.11 frame with a good FCS, bssidTxt from the parsed frame when known
    void observe(const uint8_t* p, size_t n, const std::string& bssid, int64_t wallSec, double sec);
    const std::vector<WifiClientInfo>& clients() const { return cl_; }
    const std::vector<WifiFloodAlarm>& alarms() const { return al_; }
    uint64_t deauths() const { return deauths_; }
    uint64_t handshakes() const { return eapol_; }   // EAPOL-Key frames seen: counted only
    void clear();
    std::string tagOf(const uint8_t* mac) const;
private:
    uint32_t salt_;
    std::vector<WifiClientInfo> cl_;
    std::vector<WifiFloodAlarm> al_;
    uint64_t deauths_ = 0, eapol_ = 0;
    struct Win { std::string key; double t0 = 0; int count = 0; double lastAlarm = -1e9; };
    std::vector<Win> win_;
    WifiClientInfo& client(const std::string& tag, int64_t wall);
};

} // namespace dect2
