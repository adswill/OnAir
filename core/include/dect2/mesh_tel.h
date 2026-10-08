// Mesh (LoRa: Meshtastic and MeshCore) receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
// Times are seconds of signal since the receiver started (timeSec says where "now" is); frequencies are absolute, in Hz, computed from
// the frequency the receiver was told it is tuned to (MeshReceiver::setTunedHz).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One LoRa setting the receiver searches
struct MeshDecoderInfo {
    int protocol = 1;            // 1 Meshtastic, 2 MeshCore
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
    bool decrypted = false;
    std::string note;            // why not decrypted ("encrypted (direct)", "unknown channel 0x1f", "CRC error")
    std::string detail;          // one short line about the content
    std::string path;            // MeshCore path hashes
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
    long uptimeS = -1;
    bool hasEnv = false;
    double tempC = 0, humidity = 0, pressureHpa = 0;
    float lastSnrDb = 0;
    float lastLevelDb = 0;
    int hopsAway = -1;           // hop start - hop limit of its last packet (-1 unknown)
    double lastHeard = -1;
    uint32_t packets = 0;
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
    int region = 0;              // 0 EU, 1 US
    int protocols = 3;           // 1 Meshtastic, 2 MeshCore
    bool presetSearch = false;   // all Meshtastic presets of the region, not only LongFast
    std::vector<MeshDecoderInfo> decoders;
    // tables (oldest first; at most 100 packets, 150 nodes, 100 messages: the oldest leave first, the node heard longest ago)
    std::vector<MeshPacket> packets;
    std::vector<MeshNode> nodes;
    std::vector<MeshMessage> messages;
    std::vector<MeshTypeCount> counts;
    uint64_t preambles = 0, headerBad = 0;
    std::vector<std::string> userChannels;   // "Meshtastic: name", "MeshCore: name"
};

inline std::string meshSummary(const MeshTelemetry& t) {
    char b[160];
    if (t.state == 0) return "Mesh (LoRa): searching";
    int mt = 0, mc = 0;
    for (const auto& n : t.nodes) (n.protocol == 1 ? mt : mc)++;
    snprintf(b, sizeof b, "Mesh (LoRa): %s, %llu frames, %d Meshtastic / %d MeshCore nodes, %zu messages, SNR %.1f dB",
             t.state == 2 ? "decoding" : "signal", (unsigned long long)t.blocksOk, mt, mc, t.messages.size(), t.snrDb);
    return b;
}

} // namespace dect2
