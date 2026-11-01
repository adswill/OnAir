// Mesh (LoRa) transmit: the sender's own Meshtastic identity and the packets built from it. Nothing here blocks a transmission: problems
// with the settings come back as warnings (meshTxWarnings) that the panel shows, and the packet is built anyway.
//
// A text goes out as two packets: the node's NODEINFO (long name, short name, node ID, hardware "private") so that the other nodes show who
// is calling, then the text message on the chosen channel (name + PSK) with the chosen hop limit. A ham callsign can be put in front of the
// text, which is also how a licensed operator identifies a transmission.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct MeshTxIdentity {
    std::string longName = "OnAir";           // up to 39 bytes
    std::string shortName = "ONAR";           // up to 4 characters
    std::string nodeId;                       // "!1a2b3c4d" or 8 hex digits; empty = derived from the callsign / names
    std::string channelName = "LongFast";
    std::string psk = "default";              // "default" (the public key, AQ==), "none", 32 or 64 hex digits, or base64 of 1, 16 or 32 bytes
    std::string preset = "LongFast";          // modem preset: LongFast, MediumFast, ...
    int hopLimit = 3;                         // 0 .. 7
    std::string callsign;                     // the operator's ham callsign (optional)
    bool callsignInMessage = false;           // put "<callsign>: " in front of the text
    bool sendNodeInfo = true;                 // send the NODEINFO packet before the text
};

// "default" / "none" / hex / base64 -> the key bytes (empty = not encrypted). false: the text is none of those or has a bad length (key empty).
bool meshTxParsePsk(const std::string& psk, std::vector<uint8_t>& key);
// "!1a2b3c4d" or 1a2b3c4d -> number; false: not 8 hex digits (num untouched)
bool meshTxParseNodeId(const std::string& id, uint32_t& num);
// The node number actually used: the given ID, else a stable number made from the callsign (or the long name)
uint32_t meshTxNodeNum(const MeshTxIdentity& id);
// The text as sent (callsign prefix when asked and the callsign is set)
std::string meshTxText(const MeshTxIdentity& id, const std::string& text);
// Things the user should know; the transmission is never refused for them
std::vector<std::string> meshTxWarnings(const MeshTxIdentity& id, const std::string& text);

struct MeshTxFrame {
    std::vector<uint8_t> bytes;               // the packet as it goes into the LoRa payload
    std::string what;                         // "node info", "text"
};
// The packets of one transmission (packetIdSeed: the first packet id; the text's is one more)
std::vector<MeshTxFrame> meshTxPackets(const MeshTxIdentity& id, const std::string& text, uint32_t packetIdSeed);

} // namespace dect2
