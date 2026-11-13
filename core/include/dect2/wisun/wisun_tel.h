// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-SUN FAN monitor telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

constexpr size_t kWsPanMax = 64;
constexpr size_t kWsNodeMax = 2000;
constexpr size_t kWsLinkMax = 4000;
constexpr size_t kWsLogMax = 800;
constexpr size_t kWsHopMax = 3000;      // frames kept for the hop chart
constexpr size_t kWsPpsHist = 120;      // seconds of frames-per-second history
constexpr int kWsUttTypes = 12;         // wisun::kUttTypes

// One network (PAN), from its PAN advertisements (network name, size, routing cost) and the PAN ID frames carry
struct WsPan {
    uint16_t pan = 0;
    std::string netName;               // "" until a PAN advert with the NETNAME-IE is heard
    int panSize = -1;                  // the size the border router advertises (PAN-IE)
    int panVersion = -1;
    int tpsVersion = -1;               // FAN TPS version from the PAN-IE (1: FAN 1.0, 2: FAN 1.1)
    std::string borderRouter;          // the EUI-64 advertising routing cost 0, "" not heard
    std::string unicast;               // the US-IE of the last advert (channel plan, function, dwell)
    std::string broadcast;             // the BS-IE, when an unsecured frame carried one
    std::string phyModes;              // POM-IE: "3 5"
    int nodes = 0;                     // nodes heard in this PAN
    float rssi = -200;
    uint64_t packets = 0;
    double firstSec = 0, lastSec = 0;
    int64_t lastWall = 0;
};

// One node (EUI-64)
struct WsNode {
    std::string eui;                   // "18:DF:C1:FF:FE:8B:0D:4D"
    uint64_t euiNum = 0;
    std::string vendor;                // from the OUI, "" unknown
    int pan = -1;                      // the PAN it belongs to, -1 not known
    std::string role;                  // "border router", "router" (sends PAN adverts), "joining" (solicits, EAPOL), "LFN", "node"
    int routingCost = -1;              // from its own PAN advert
    int tpsVersion = -1;
    float rssi = -200;                 // dBFS of its last frame (the radio's level, not calibrated dBm)
    float snrDb = 0;
    uint64_t packets = 0;
    uint64_t perUtt[kWsUttTypes] = {};
    uint64_t encrypted = 0;
    double lastMhz = 0;                // where its last frame was
    std::vector<uint8_t> channels;     // a mark per channel of the band plan it was heard on (WisunTelemetry::chanMhz)
    int bitRate = 0;                   // of its last frame
    std::string unicast;               // its US-IE (from an unsecured frame)
    std::string broadcast;             // its BS-IE
    std::string phyModes;
    std::string authenticator;         // EA-IE of its EAPOL frames
    std::string eapState;              // the last EAP step ("EAP request TLS", "EAPOL-Key", ...)
    uint32_t frameCounter = 0;         // the last MAC security frame counter
    double firstSec = 0, lastSec = 0;
    int64_t lastWall = 0;
    std::string last;                  // the last frame's one-line summary
};

// A directed link a > b: unicast frames from a to b, acks from b with the level b reported for a's frames (RSL-IE)
struct WsLink {
    std::string src, dst;
    uint64_t srcNum = 0, dstNum = 0;
    uint64_t frames = 0;               // unicast frames a > b
    uint64_t acks = 0;                 // acks b > a
    int rslDbm = -999;                 // the last RSL b reported for a (dBm, the sender's own estimate)
    float rssi = -200;                 // a's level here
    double lastSec = 0;
    int64_t lastWall = 0;
};

struct WsFrame {
    int64_t wallTime = 0;
    double sec = 0;
    double mhz = 0;
    int channel = -1;                  // in the plan the frame's bit rate belongs to (-1 off the plan)
    int opClass = 0;
    int bitRate = 0;
    int utt = -1;                      // Wi-SUN frame type (-1: none, plain 802.15.4)
    std::string type;                  // "PAN Advert", ...
    std::string src, dst;
    int pan = -1;
    int len = 0;                       // PSDU octets, FCS included
    float rssi = -200, snrDb = 0;
    bool encrypted = false;
    std::string ies;
    std::string info;
    std::string hex;                   // the MPDU (FCS excluded), hexadecimal
};

// One frame for the hop chart: when, where and what
struct WsHop {
    double sec = 0;
    float mhz = 0;
    int utt = -1;
    int node = -1;                     // index into a colour table (the source's EUI hashed), -1 none
};

struct WisunTelemetry {
    uint64_t seq = 0;
    int state = 0;                     // 0 nothing heard, 1 frames heard in the last seconds
    double cfoHz = 0;                  // carrier offset of the last good frame
    float snrDb = 0;
    float levelDb = -200;              // input level, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;             // frames with a good FCS
    uint64_t blocksBad = 0;            // start-of-frame and header found, FCS failed
    uint64_t corrected = 0;            // good frames that needed bits flipped (the least reliable ones, checked by the CRC)
    // details
    double timeSec = 0, inputRate = 0;
    std::string region;                // the band plan's id: "ANZ", "NA", "EU863", ... (wisun::bandPlans())
    std::string bandName;              // "Australia / New Zealand"
    double bandLoMhz = 0, bandHiMhz = 0;   // the band plan's band
    double loMhz = 0, hiMhz = 0;       // the part of the band the receiver covers (0: not known, a file without a frequency)
    int bins = 0;                      // channelizer bins
    double binKhz = 0;
    int channelsCovered = 0;           // channels of the 200 kHz grid inside the coverage
    std::vector<double> chanMhz;       // every channel centre of the band plan (all its spacings, in order): what perChannel and WsNode::channels count
    uint64_t bursts = 0;               // bursts the energy detector handed to the demodulators
    uint64_t dropped = 0;              // bursts the decoder had no time for
    uint64_t modeSwitches = 0;         // mode switch PHRs: the frame that follows is in another PHY (FAN 1.1 OFDM / faster FSK), not decoded
    std::vector<std::pair<int, uint32_t>> switchModes;   // the PHY mode each switched to (raw PHR field), and how often
    uint64_t fecFrames = 0;            // frames with the FEC start-of-frame (not decoded)
    uint64_t perUtt[kWsUttTypes] = {};
    uint64_t plain = 0;                // 802.15.4 frames without Wi-SUN IEs
    uint64_t encryptedFrames = 0;
    std::vector<uint32_t> perChannel;  // good frames per channel (chanMhz)
    float pps = 0;
    std::vector<float> ppsHist;
    float loadPct = 0;                 // the receiver's time against real time
    std::vector<WsPan> pans;           // most recently heard first
    std::vector<WsNode> nodes;         // most recently heard first
    std::vector<WsLink> links;         // most recently heard first
    std::vector<WsFrame> log;          // newest first, at most kWsLogMax
    std::vector<WsHop> hops;           // oldest first, at most kWsHopMax
};

inline std::string wisunSummary(const WisunTelemetry& t) {
    char b[200];
    if (t.blocksOk == 0) {
        if (t.loMhz > 0) { snprintf(b, sizeof b, "listening %.1f - %.1f MHz", t.loMhz, t.hiMhz); return b; }
        return "listening for Wi-SUN frames";
    }
    snprintf(b, sizeof b, "%zu networks, %zu nodes, %zu links, %llu frames", t.pans.size(), t.nodes.size(), t.links.size(), (unsigned long long)t.blocksOk);
    return b;
}

} // namespace dect2
