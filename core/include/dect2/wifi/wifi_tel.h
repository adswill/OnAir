// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi network monitor telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include "dect2/wifi/wifi_clients.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kWifiRssiHist = 24;   // the last levels of a network, oldest first
constexpr size_t kWifiApMax = 300;     // networks kept (the longest unheard go first)
constexpr size_t kWifiLogMax = 400;    // frames in the log

// One access point (a BSSID): what its beacons and probe responses announce
struct WifiAp {
    std::string bssid;                 // "00:04:0E:12:34:56"
    std::string ssid;                  // "" when hidden
    bool hidden = false;               // the beacons carry no name
    std::string vendor;                // from the OUI ("local": a locally administered, often virtual BSSID)
    int channel = 0;                   // the AP's own (DS parameter set), 0 not sent
    int heardOn = 0;                   // the channel the radio was on (0: a file)
    int security = 0;                  // wifi::Security
    bool enterprise = false;
    std::string ciphers, rates;
    int interval = 0;                  // beacon interval, TU (1.024 ms)
    uint16_t cap = 0;
    float rssi = -200;                 // dBFS of the last frame (the radio's level, not calibrated dBm)
    std::vector<float> rssiHist;
    double firstSec = 0, lastSec = 0;  // signal time
    int64_t firstWall = 0, lastWall = 0;
    uint64_t beacons = 0, probeResps = 0;
    // 5 / 6 GHz and the capabilities (wifi_elems.h)
    int band = 0;                      // 0 2.4 GHz, 1 5 GHz, 2 6 GHz: where it was heard
    int widthMhz = 20;                 // the BSS channel width from HT / VHT / HE / EHT operation
    int phy = 0;                       // the PHY of the last frame: 0 802.11b DSSS, 1 OFDM
    std::string standard;              // "a/n/ac/ax": what the elements announce
    std::string caps;                  // plain words: "Wi-Fi 6 (HE) 2x2, up to 80 MHz wide, up to 1201 Mbit/s, TWT, BSS color 5"
    std::string akms;                  // "SAE", "PSK, SAE" ... the key management suites
    int pmf = 0;                       // management frame protection: 0 none, 1 capable, 2 required
    int bssColor = -1;                 // HE BSS color (-1 none)
    bool twt = false;                  // HE: target wake time supported
    bool eht = false;                  // Wi-Fi 7
    std::string country;               // "DE I"
    int powerConstraint = -1;          // dB
    int mdid = -1;                     // mobility domain
    int he6Reg = -1;                   // 6 GHz: 0 indoor, 1 standard power, 2 very low power AP
    std::string neighbors;             // the Reduced Neighbor Report in words (the co-located 6 GHz APs on a 2.4 / 5 GHz beacon)
    bool fils = false;                 // FILS discovery frames heard (6 GHz)
    bool unsolicited = false;          // unsolicited probe responses heard (6 GHz)
    uint64_t filsFrames = 0;
};

struct WifiFrameLog {
    int64_t wallTime = 0;
    double sec = 0;
    int channel = 0;                   // the tuned channel (0: a file)
    int band = 0;
    int type = 0, subtype = 0;
    std::string bssid;
    float rssi = -200;
    int len = 0;                       // MPDU bytes, FCS included
    std::string info;
};

struct WifiTelemetry {
    uint64_t seq = 0;
    int state = 0;                     // 0 nothing heard, 1 frames heard in the last seconds
    double cfoHz = 0;                  // carrier offset of the last good frame
    float snrDb = 0;                   // symbol SNR after despreading (from the DBPSK phase spread)
    float levelDb = -200;              // input level, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;             // frames with a good FCS
    uint64_t blocksBad = 0;            // good PLCP header, bad FCS
    // details
    double timeSec = 0, inputRate = 0;
    int channel = 0;                   // the channel the radio is tuned to (0: not known, a file)
    uint64_t sfdHits = 0, headersOk = 0, headersBad = 0, otherRate = 0;   // otherRate: a good header for 2, 5.5 or 11 Mbit/s (not decoded)
    uint64_t perType[3] = {};          // good frames: management, control, data (data frames are only counted)
    uint64_t beacons = 0, probeResps = 0;
    int band = 0;                      // the band the radio is tuned to (from its centre): 0 2.4, 1 5, 2 6 GHz
    int subChannels = 1;               // 20 MHz channels listened to at once (a wide capture on 5 / 6 GHz: several)
    int phy = 0;                       // the PHY of the last good frame: 0 802.11b DSSS, 1 OFDM
    uint64_t ofdmFrames = 0;           // good-FCS frames from the OFDM receiver
    uint64_t ofdmTriggers = 0, ofdmSignalOk = 0, ofdmSignalBad = 0, ofdmOther = 0, ofdmBadFcs = 0;   // the receiver's stages: preambles, SIGNAL fields, FCS
    uint64_t dupFrames = 0;            // frames heard on the secondary 20 MHz of a wider channel (non-HT duplicates), not counted twice
    uint64_t filsFrames = 0;
    std::vector<WifiAp> aps;           // most recently heard first
    std::vector<WifiClientInfo> clients;   // stations, tagged by a salted hash, most recently heard last
    std::vector<WifiFloodAlarm> floodAlarms;
    uint64_t deauthFrames = 0, handshakeFrames = 0;   // EAPOL-Key frames: a count, never captured
    std::vector<WifiFrameLog> log;     // beacons and probe responses, newest first
};

inline std::string wifiSummary(const WifiTelemetry& t) {
    char b[160];
    if (t.aps.empty()) {
        if (t.channel) { snprintf(b, sizeof b, "listening on channel %d", t.channel); return b; }
        return "listening for beacons";
    }
    snprintf(b, sizeof b, "%zu networks, %llu beacons", t.aps.size(), (unsigned long long)t.beacons);
    return b;
}

} // namespace dect2
