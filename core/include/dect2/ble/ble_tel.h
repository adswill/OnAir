// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Bluetooth LE advertising monitor telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kBleRssiHist = 24;    // the last levels of a device, oldest first
constexpr size_t kBleDevMax = 400;     // devices kept (the longest unheard go first)
constexpr size_t kBleLogMax = 400;     // packets in the log

// One advertiser (an address and its type). Random private addresses change every few minutes, so one phone can show up as several.
struct BleDevice {
    std::string addr;                  // "C0:FF:EE:12:34:56"
    bool random = false;
    int kind = 0;                      // ble::addrKind: 0 public, 1 random static, 2 resolvable private, 3 non-resolvable private, 4 random
    std::string name, vendor, beacon, services;
    int company = -1;
    int txPower = 127;                 // 127: not sent
    int flags = -1;
    bool connectable = false, scannable = false, extended = false, scanned = false;   // scanned: something sent it a scan request
    float rssi = -200;                 // dBFS of the last packet (the radio's level, not calibrated dBm)
    std::vector<float> rssiHist;
    double firstSec = 0, lastSec = 0;  // signal time
    int64_t firstWall = 0, lastWall = 0;
    uint64_t packets = 0;
    uint32_t pduMask = 0;              // 1 << PDU type
    uint8_t chanMask = 0;              // bit 0 channel 37, bit 1 38, bit 2 39
    std::string mfrHex;                // the last manufacturer data (after the company id)
};

struct BlePacket {
    int64_t wallTime = 0;
    double sec = 0;
    int channel = 0;                   // 37, 38 or 39 (found from the whitening)
    int type = 0;                      // ble::PduType
    std::string addr;                  // AdvA ("" when not sent)
    bool random = false;
    float rssi = -200;
    int len = 0;
    std::string info;                  // ble::Adv::summary
};

struct BleTelemetry {
    uint64_t seq = 0;
    int state = 0;                     // 0 nothing heard, 1 packets heard in the last seconds
    double cfoHz = 0;                  // carrier offset of the last good packet
    float snrDb = 0;                   // eye SNR of the last good packets (discriminator output)
    float levelDb = -200;              // input level, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;             // packets with a good CRC
    uint64_t blocksBad = 0;            // access address found, CRC failed
    // details
    double timeSec = 0, inputRate = 0;
    int channel = 0;                   // the advertising channel the radio is tuned to (0: not known, a file)
    int lastChannel = 0;               // the channel of the last good packet
    float deviationHz = 0;
    uint64_t aaHits = 0, perType[8] = {};
    uint64_t perChannel[3] = {};       // good packets on 37, 38, 39
    std::vector<BleDevice> devices;    // most recently heard first
    std::vector<BlePacket> log;        // newest first, at most kBleLogMax
};

inline std::string bleSummary(const BleTelemetry& t) {
    char b[160];
    if (t.blocksOk == 0) {
        if (t.channel) { snprintf(b, sizeof b, "listening on channel %d", t.channel); return b; }
        return "listening for advertising";
    }
    snprintf(b, sizeof b, "%zu devices, %llu packets", t.devices.size(), (unsigned long long)t.blocksOk);
    return b;
}

} // namespace dect2
