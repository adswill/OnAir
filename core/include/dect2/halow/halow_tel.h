// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (802.11ah) monitor telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kHlNetMax = 200;
constexpr size_t kHlLogMax = 600;
constexpr size_t kHlPpsHist = 120;     // seconds of packets-per-second history

// One network (a BSSID), from its beacons and probe responses
struct HlNet {
    std::string bssid;                 // "AA:BB:CC:DD:EE:FF"
    std::string vendor;                // from the OUI, "" unknown
    std::string vendorIe;              // OUIs of the vendor specific elements, with the maker's name where known ("0C:BF:74 Morse Micro"), "" none
    double primaryMhz = 0;             // centre of the 1 MHz primary channel from the S1G Operation element (class start + 0.5 MHz x primary number), 0 unknown
    std::string ssid;                  // "" when only the compressed form (comp) was sent
    bool hasSsid = false;
    bool hidden = false;               // an empty SSID element
    uint32_t comp = 0;                 // the 32-bit compressed SSID of an S1G beacon, 0 none
    int ccf = 0;                       // S1G channel centre frequency number (0: not announced)
    int opClass = 0;
    double freqMhz = 0;                // centre of the operating channel, or of the radio when the beacon does not say
    int opWidthMhz = 0;                // operating bandwidth from the S1G Operation element (0 not announced)
    int seenWidthMhz = 0;              // the PPDU bandwidth the frame was received in: 1, 2, 4, 8 or 16
    int primaryWidthMhz = 0;           // primary channel width from the S1G Operation element (1 or 2; 0 not announced)
    int mcs = 0;                       // of the last frame
    int nss = 1;                       // spatial streams of the last frame (from its SIG field)
    int pre = 2;                       // preamble of the last frame: 0 S1G_SHORT, 1 S1G_LONG, 2 S1G_1M
    float snrDb = 0;                   // tone SNR of the last frame
    std::string country;               // from a Country element, "" none
    int dtimPeriod = 0;                // TIM element (0: none)
    bool hasRaw = false, hasTwt = false;   // RAW / TWT elements seen
    int txPowerDbm = -1;               // power constraint element (-1: none)
    int longPreamble = -1;             // S1G Capabilities: long preamble supported (-1 not announced)
    int basicMcs = -1;                 // basic MCS map for one stream: 0 MCS 0-2, 1 MCS 0-7, 2 MCS 0-9 (-1 not announced)
    float rateMbps = 0;                // PHY rate of the last frame
    std::string security;              // information only: "Open", "WPA2-PSK", "WPA3-SAE", ...
    int intervalTu = 0;                // beacon interval (1 TU = 1.024 ms)
    float rssi = -200;                 // dBFS of the last frame (the radio's level, not calibrated dBm)
    float rssiMax = -200;
    uint64_t beacons = 0;
    uint64_t packets = 0;
    double firstSec = 0, lastSec = 0;  // signal time
    int64_t lastWall = 0;
    std::string last;                  // the last frame's one-line summary
};

struct HlFrame {
    int64_t wallTime = 0;
    double sec = 0;
    int widthMhz = 0;
    int mcs = 0;
    int nss = 1;                       // spatial streams
    int pre = 2;                       // preamble: 0 S1G_SHORT, 1 S1G_LONG, 2 S1G_1M
    bool sgi = false;
    float snrDb = 0;
    float rateMbps = 0;
    int len = 0;                       // PSDU bytes, FCS included
    bool decoded = false;              // the PSDU was decoded and its FCS passed
    float rssi = -200;
    std::string type;                  // S1G beacon, probe response, data, PPDU ...
    std::string bssid;
    std::string info;
};

struct HalowTelemetry {
    uint64_t seq = 0;
    int state = 0;                     // 0 nothing heard, 1 frames heard in the last seconds
    double cfoHz = 0;                  // carrier offset of the last decoded PPDU
    float snrDb = 0;                   // tone SNR of the last PPDUs
    float levelDb = -200;              // input level, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;             // frames with a good FCS
    uint64_t blocksBad = 0;            // PPDU decoded, FCS failed
    // details
    double timeSec = 0, inputRate = 0;
    double centerMhz = 0;              // where the radio is tuned (0: not known, a file)
    int chanHint = 0;                  // the S1G channel number that frequency is in the EU or US plan (0 none)
    uint64_t preambles = 0;            // STF detections
    uint64_t sigOk = 0, sigBad = 0;    // SIG field CRC passed / no width produced a valid SIG
    uint64_t undecoded = 0;            // PPDUs whose SIG was read but whose data rate is not decoded here (counted, shown in the log)
    uint64_t perWidth[5] = {};         // good SIG fields at 1, 2, 4, 8, 16 MHz
    uint64_t perMcs[11] = {};          // good SIG fields per MCS 0 - 10
    uint64_t perNss[5] = {};           // per spatial stream count 1 - 4 (index 1 - 4)
    uint64_t perPre[3] = {};           // S1G_SHORT, S1G_LONG, S1G_1M
    uint64_t ldpcFrames = 0, stbcFrames = 0, mimoFrames = 0, aggFrames = 0, ndpFrames = 0;   // PPDUs read from the SIG field only, by reason
    uint64_t dataBad = 0;              // data field could not be read (cut short, length)
    int maxWidthMhz = 0;               // widest channel the radio's sample rate can hold (0: rate too low for any)
    int widthMask = 31;                // which widths are tried (bit 0 = 1 MHz ... bit 4 = 16 MHz)
    int region = -1;                   // the plan the tuned frequency belongs to (halow_chan.h), -1 none
    uint64_t beaconsSeen = 0;
    float pps = 0;                     // frames per second, the last second
    std::vector<float> ppsHist;
    std::vector<HlNet> nets;           // most recently heard first
    std::vector<HlFrame> log;          // newest first, at most kHlLogMax
};

// One line for the status bar and the top bar.
inline std::string halowSummary(const HalowTelemetry& t) {
    char b[160];
    if (t.blocksOk == 0 && t.sigOk == 0) {
        if (t.preambles) { snprintf(b, sizeof b, "%llu preambles, no frame decoded yet", (unsigned long long)t.preambles); return b; }
        if (t.centerMhz > 0) { snprintf(b, sizeof b, "listening on %.1f MHz", t.centerMhz); return b; }
        return "listening for HaLow";
    }
    snprintf(b, sizeof b, "%zu networks, %llu frames", t.nets.size(), (unsigned long long)(t.blocksOk + t.undecoded));
    return b;
}

} // namespace dect2
