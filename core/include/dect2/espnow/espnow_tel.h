// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ESP-NOW telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kEspnowRssiHist = 24;   // the last levels of a device, oldest first
constexpr size_t kEspnowDevMax = 300;    // devices kept (the longest unheard go first)
constexpr size_t kEspnowLogMax = 500;    // messages in the log
constexpr size_t kEspnowKeepBytes = 1470; // payload bytes kept per message (ESP-NOW v2 maximum)

// One sender (the source address of its frames)
struct EspnowDevice {
    std::string mac;
    std::string vendor;                // from the OUI table ("Espressif" for most ESP8266 / ESP32 chips); "local" for a locally administered address
    bool espressif = false;            // the address's OUI is one of Espressif's
    int version = -1;                  // ESP-NOW version of its last frame
    uint64_t packets = 0, encrypted = 0, broadcasts = 0;
    float rate = 0;                    // packets per second over the last seconds
    float rssi = -200;                 // dBFS
    std::vector<float> rssiHist;
    int channel = 0;                   // Wi-Fi channel it was heard on (0: not known)
    int rateKbps = 0;                  // PHY rate of its last frame
    std::string lastTo;                // the destination of its last frame
    std::vector<uint8_t> lastPayload;  // the body of its last readable frame
    bool lastEncrypted = false;        // the last frame was encrypted and not decrypted
    double firstSec = 0, lastSec = 0;  // signal time
    int64_t firstWall = 0, lastWall = 0;
    double winStart = 0;               // packet-rate window: its start (signal time)
    uint32_t winCount = 0;             // and the packets in it
};

struct EspnowMsg {
    int64_t wallTime = 0;
    double sec = 0;
    int channel = 0;
    int rateKbps = 1000;
    bool shortPreamble = false;
    std::string from, to;
    uint16_t seq = 0;
    int version = -1;
    bool encrypted = false, decrypted = false;
    bool fcsOk = true;                 // false: shown only with "show frames with a bad FCS"; never counted as a device
    float rssi = -200;
    float snrDb = 0;
    double cfoHz = 0;
    std::vector<uint8_t> payload;      // the ESP-NOW body (up to kEspnowKeepBytes); empty when encrypted
    std::string info;                  // espnow::summary
};

struct EspnowTelemetry {
    uint64_t seq = 0;
    int state = 0;                     // 0 nothing heard, 1 ESP-NOW heard in the last seconds
    double cfoHz = 0;                  // carrier offset of the last good frame
    float snrDb = 0;
    float levelDb = -200;              // input level, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;             // ESP-NOW frames decoded (FCS good)
    uint64_t blocksBad = 0;            // frames whose FCS failed (all 802.11 frames the demodulator framed)
    // details
    double timeSec = 0, inputRate = 0;
    double centerMhz = 0;              // the radio's centre (0: a file)
    int channel = 0;                   // the Wi-Fi channel listened to (0: a file)
    double offsetHz = 0;               // where it lies from the centre
    uint64_t wifiFrames = 0;           // all good 802.11 frames
    uint64_t actionFrames = 0;         // of them, action frames
    uint64_t espnowFrames = 0, encryptedFrames = 0, decryptedFrames = 0, broadcastFrames = 0;
    uint64_t perRate[5] = {};          // ESP-NOW frames by PHY: DSSS 1, DSSS 2 long, DSSS 2 short, OFDM 6, OFDM 12 Mbit/s
    uint64_t perChannel[15] = {};      // ESP-NOW frames per Wi-Fi channel (index 1 - 14)
    std::vector<EspnowDevice> devices; // most recently heard first
    std::vector<EspnowMsg> log;        // newest first
};

inline std::string espnowSummary(const EspnowTelemetry& t) {
    char b[160];
    if (t.devices.empty()) return t.channel ? "listening on channel " + std::to_string(t.channel) : "listening for ESP-NOW";
    snprintf(b, sizeof b, "%zu device%s, %llu frames", t.devices.size(), t.devices.size() == 1 ? "" : "s", (unsigned long long)t.espnowFrames);
    return b;
}

} // namespace dect2
