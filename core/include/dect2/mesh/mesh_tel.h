// Mesh (LoRa: Meshtastic and MeshCore) receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
// Times are seconds of signal since the receiver started (timeSec says where "now" is); frequencies are absolute, in Hz, computed from
// the frequency the receiver was told it is tuned to (MeshReceiver::setTunedHz).
#pragma once
#include "dect2/packet/packet_tel.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One LoRa setting the receiver searches
struct MeshDecoderInfo {
    int protocol = 1;            // 1 Meshtastic, 2 MeshCore, 3 LoRa APRS, 4 MeshCom, 5 LoRaWAN
    std::string preset;          // "LongFast", "MediumFast", ..., "MeshCore EU"
    double freqHz = 0;           // channel centre
    double offsetHz = 0;         // from the tuned frequency
    int sf = 11;
    double bwHz = 250000;
    int cr = 5;                  // 5..8 for 4/5..4/8 (what the network uses; each frame's header says its own)
    int syncWord = 0x2B;
    int preamble = 16;
    bool ldro = false;
    bool inBand = true;          // false: outside the captured band, not searched
    uint64_t frames = 0, crcBad = 0, headerBad = 0, syncBad = 0;
    float lastSnrDb = 0;
    double lastCfoHz = 0;
    double lastFrameSec = -1;
};

// One received LoRa frame
struct MeshPacket {
    double timeSec = 0;
    double unixTime = 0;         // the computer clock when it was decoded (0: not known): lost samples and Stop / Start do not move it
    int protocol = 1;
    std::string preset;
    double freqHz = 0;           // channel centre + measured carrier offset
    double cfoHz = 0;
    int sf = 0, cr = 0;
    double bwHz = 0;
    float levelDb = 0;           // signal power in dB relative to full scale (RSSI-like, not calibrated)
    float snrDb = 0;             // in the LoRa bandwidth
    int size = 0;                // payload bytes
    bool crcOk = false;
    bool parsed = false;         // the packet layer understood it
    std::string type;            // "TEXT_MESSAGE_APP", "ADVERT", "GRP_TXT", ...; empty when the CRC failed
    std::string from, to, channel;
    uint32_t packetId = 0;
    int hopLimit = -1, hopStart = -1;
    bool viaMqtt = false;        // Meshtastic: the packet came into the mesh from MQTT (a gateway put it on the air)
    int nextHop = -1, relayNode = -1;   // Meshtastic: last byte of the node number of the next hop and of the node that sent this copy (0 = none)
    bool decrypted = false;
    std::string note;            // why not decrypted ("encrypted (direct)", "unknown channel 0x1f", "CRC error")
    std::string detail;          // one short line about the content
    std::string path;            // MeshCore path hashes; LoRa APRS / MeshCom: the digipeaters or relays
    std::string raw;             // LoRa APRS / MeshCom: the payload as printable text (other bytes as \xNN); LoRaWAN: the bytes in hex
    // LoRaWAN (protocol 5): type = MType name, from = DevAddr or DevEUI, to = JoinEUI, packetId = FCnt
    std::string network;         // from the DevAddr's NetID ("The Things Network", "NetID 0x000005")
    int mtype = -1;              // MHDR MType (0 join request ... 7 proprietary)
    int fport = -1;              // -1 none
    int frmLen = -1;             // encrypted FRMPayload bytes
    std::string flags;           // "ADR, ACK", ...
    std::string mac;             // FOpts MAC command names, comma separated
    uint32_t mic = 0;
};

// A LoRaWAN end device: by DevAddr (data frames) or, until it has one, by the DevEUI of its join requests
struct MeshLorawanDevice {
    std::string id;              // "26011A2B" (DevAddr) or "DevEUI A84041..."
    std::string devAddr, devEui, joinEui, vendor, network;
    double lastHeard = -1;
    double freqHz = 0;           // of the last frame
    float lastSnrDb = 0, lastLevelDb = 0;
    int sf = 0;
    long fcnt = -1;              // last frame counter (-1 none)
    uint32_t packets = 0, uplinks = 0, downlinks = 0, joins = 0, confirmed = 0;
    uint32_t missed = 0;         // gaps in the frame counter (frames sent but not heard)
    int lastFport = -1;
    bool adr = false;
    std::string lastType, lastMac;
    int battery = -1;            // DevStatusAns battery byte: 0 external power, 1 - 254 level, 255 unknown; -1 never sent
    int marginDb = 0;            // DevStatusAns link margin
};

// A LoRa APRS or MeshCom station (the packet mode's station record, plus what the LoRa layer knows)
struct MeshAprsStation {
    int protocol = 3;            // 3 LoRa APRS, 4 MeshCom
    PacketStation st;            // call, symbol, position, comment, last heard (signal seconds), frame count
    std::string path;            // path of its last packet
    std::string lastType;        // APRS type of its last packet ("Position", "Message", ...; MeshCom "TEXT", "POSITION")
    float lastSnrDb = 0;
    int batteryPct = -1;         // MeshCom "/B="
};

struct MeshNode {
    int protocol = 1;
    std::string id;              // Meshtastic "!1a2b3c4d"; MeshCore public-key prefix in hex
    std::string longName, shortName, hwModel, role;
    bool hasPosition = false;
    double lat = 0, lon = 0, altM = 0;
    double posSec = -1;          // when the position arrived
    uint32_t posUnix = 0;        // the time sent with it (0 = none)
    double batteryPct = -1, voltage = -1;
    double channelUtilPct = -1, airUtilTxPct = -1;
    int64_t uptimeS = -1;   // a uint32 from the radio: long is 32 bit on Windows
    bool hasEnv = false;
    double tempC = 0, humidity = 0, pressureHpa = 0;
    float lastSnrDb = 0;
    float lastLevelDb = 0;
    int hopsAway = -1;           // hop start - hop limit of its last packet (-1 unknown)
    double lastHeard = -1;
    uint32_t packets = 0;
    double firstHeard = -1;
    float snrMin = 0, snrMax = 0;
    double snrSum = 0;           // over `packets`: the mean SNR is snrSum / packets
    uint32_t encrypted = 0;      // of its packets, the ones that could not be decrypted
    double envSec = -1, devSec = -1;   // when the last environment / device (battery) telemetry arrived
};

// One report a node sent about itself (Meshtastic TELEMETRY_APP device or environment metrics, POSITION_APP; MeshCore advert positions):
// the history behind the node's last values
struct MeshReading {
    double timeSec = 0;
    double unixTime = 0;         // the computer clock when it was decoded (0: not known)
    int protocol = 1;
    std::string id;              // the node
    double batteryPct = -1, voltage = -1, channelUtilPct = -1, airUtilTxPct = -1;   // -1 = not in this report
    int64_t uptimeS = -1;   // a uint32 from the radio: long is 32 bit on Windows
    bool hasEnv = false;
    double tempC = 0, humidity = 0, pressureHpa = 0;
    bool hasPosition = false;
    double lat = 0, lon = 0, altM = 0;
};

struct MeshMessage {
    double timeSec = 0;
    int protocol = 1;
    std::string channel;         // "LongFast" (Meshtastic default), "Public" (MeshCore), a user channel, or a hash
    std::string from;            // node id, or the sender name MeshCore puts in the text
    std::string fromName;        // long name when the node is known
    std::string to;              // "^all" / node id
    std::string text;
    int hops = -1;
    uint32_t packetId = 0;
};

struct MeshTypeCount {
    int protocol = 1;
    std::string type;
    uint32_t count = 0;
};

struct MeshTelemetry {
    uint64_t seq = 0;            // grows with every report
    int state = 0;               // 0 searching, 1 signal (a preamble lately), 2 decoding (a good frame in the last minute)
    double cfoHz = 0;            // carrier error of the last good frame
    float snrDb = 0;             // SNR of the last good frame
    bool dataValid = false;      // a frame with a good CRC has been decoded
    uint64_t blocksOk = 0;       // frames with a good CRC
    uint64_t blocksBad = 0;      // frames whose header was right but whose CRC failed
    // set-up
    double timeSec = 0;          // signal seconds since the start (now)
    double inputRate = 0;
    double tunedHz = 0;
    int region = 0;              // 0 EU, 1 US, 2 ANZ
    int protocols = 15;          // 1 Meshtastic, 2 MeshCore, 4 LoRa APRS, 8 MeshCom, 16 LoRaWAN
    bool presetSearch = false;   // all Meshtastic presets of the region, not only LongFast
    std::string mtRegion;        // Meshtastic region of the plan ("EU_868", ...)
    bool custom = false;         // a custom LoRa setting replaces the plan
    std::vector<MeshDecoderInfo> decoders;
    // tables (oldest first; at most 100 packets, 150 nodes, 100 messages: the oldest leave first, the node heard longest ago)
    std::vector<MeshPacket> packets;
    std::vector<MeshNode> nodes;
    std::vector<MeshMessage> messages;
    std::vector<MeshReading> readings;       // telemetry and position reports, oldest first (at most 2000)
    std::vector<MeshTypeCount> counts;
    uint64_t preambles = 0, headerBad = 0;
    std::vector<std::string> userChannels;   // "Meshtastic: name", "MeshCore: name"
    std::vector<MeshAprsStation> aprsStations;   // LoRa APRS and MeshCom stations and objects (at most 150; the one heard longest ago leaves)
    std::vector<MeshLorawanDevice> lorawanDevices; // LoRaWAN end devices (at most 200; the one heard longest ago leaves)
};

inline std::string meshSummary(const MeshTelemetry& t) {
    char b[220];
    if (t.state == 0) return "Mesh (LoRa): searching";
    int mt = 0, mc = 0;
    for (const auto& n : t.nodes) (n.protocol == 1 ? mt : mc)++;
    if (!t.lorawanDevices.empty() && t.nodes.empty() && t.aprsStations.empty())
        snprintf(b, sizeof b, "Mesh (LoRa): %s, %llu frames, %zu LoRaWAN devices, SNR %.1f dB", t.state == 2 ? "decoding" : "signal", (unsigned long long)t.blocksOk,
                 t.lorawanDevices.size(), t.snrDb);
    else if (!t.aprsStations.empty())
        snprintf(b, sizeof b, "Mesh (LoRa): %s, %llu frames, %d Meshtastic / %d MeshCore nodes, %zu APRS stations, %zu messages, SNR %.1f dB",
                 t.state == 2 ? "decoding" : "signal", (unsigned long long)t.blocksOk, mt, mc, t.aprsStations.size(), t.messages.size(), t.snrDb);
    else
        snprintf(b, sizeof b, "Mesh (LoRa): %s, %llu frames, %d Meshtastic / %d MeshCore nodes, %zu messages, SNR %.1f dB",
                 t.state == 2 ? "decoding" : "signal", (unsigned long long)t.blocksOk, mt, mc, t.messages.size(), t.snrDb);
    return b;
}

} // namespace dect2
