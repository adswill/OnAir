// Meshtastic and MeshCore packet layer (no radio): decoding of LoRa payloads with the published default keys and
// user-added keys, the LoRa settings of both protocols, and packet builders for the test signal.
#pragma once
#include "packet_aprs.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

enum class MeshProtocol { Meshtastic = 1, MeshCore = 2, LoraAprs = 3, MeshCom = 4 };

// what the LoRa layer knows about a received payload
struct MeshRadioInfo {
    double freqHz = 0, bwHz = 0;
    int sf = 0, cr = 0;                 // cr: 5..8 for 4/5..4/8
    double snrDb = 0, levelDb = 0, timeSec = 0;
};

struct MeshPacketInfo {
    MeshProtocol protocol = MeshProtocol::Meshtastic;
    std::string type;                   // "TEXT_MESSAGE_APP", "POSITION_APP", ..., "ADVERT", "GRP_TXT", "TXT_MSG", ...
    std::string from, to;               // Meshtastic "!1a2b3c4d" / "^all"; MeshCore public-key prefix in hex
    uint32_t packetId = 0;
    int hopLimit = -1, hopStart = -1;
    std::string channel;                // channel name when known, else its hash in hex
    bool decrypted = false;
    std::string note;                   // why not decrypted, or a remark ("encrypted (direct)", "unknown channel 0x1f", "MAC mismatch")
    size_t size = 0;
    // added later (the fields above keep their meaning)
    std::string detail;                 // one short line about the content ("battery 87 %, 4.07 V", "repeater, 3 hops")
    int portnum = -1;                   // Meshtastic port number of the decoded Data
    int channelHash = -1;               // Meshtastic header channel byte, MeshCore group channel hash
    bool wantAck = false, viaMqtt = false;
    int nextHop = -1, relayNode = -1;   // Meshtastic: last byte of a node number (0 = none)
    int routeType = -1, payloadType = -1, payloadVersion = -1;   // MeshCore header fields
    int pathHashCount = -1, pathHashSize = 0;                    // MeshCore path of a flood or direct packet
    std::string path;                   // MeshCore path hashes in hex, comma separated
};

struct MeshNodeUpdate {
    MeshProtocol protocol = MeshProtocol::Meshtastic;
    std::string nodeId, longName, shortName, hwModel, role;   // empty = not in this packet
    bool hasPosition = false;
    double lat = 0, lon = 0, altM = 0;
    double batteryPct = -1, voltage = -1;                       // -1 = not in this packet
    bool hasEnv = false;
    double tempC = 0, humidity = 0, pressureHpa = 0;
    // added later
    std::string publicKey;              // MeshCore: full public key in hex
    uint32_t positionTime = 0;          // unix time sent with the position (0 = none)
    double channelUtilPct = -1, airUtilTxPct = -1;    // -1 = not in this packet
    long uptimeS = -1;
    int sats = -1;
    bool signatureChecked = false, signatureOk = false;   // MeshCore adverts
};

struct MeshTextMessage {
    MeshProtocol protocol = MeshProtocol::Meshtastic;
    std::string channel, from, to, text;
    int hops = -1;
    // added later
    uint32_t packetId = 0, replyId = 0; // Meshtastic
    uint32_t senderTime = 0;            // MeshCore: the sender's timestamp
};

struct MeshDecodeResult {
    bool ok = false;                    // the packet was parsed (even if it could not be decrypted)
    MeshPacketInfo packet;
    std::vector<MeshNodeUpdate> nodes;
    std::vector<MeshTextMessage> messages;
    // added later: LoRa APRS and MeshCom carry APRS-style text
    bool hasAprs = false;
    std::string aprsSource, aprsDest, aprsPath;   // TNC2 header: "SRC>DEST,PATH"; MeshCom: source path (relays) and destination
    std::string aprsInfo;               // the APRS information field (MeshCom: type character + payload)
    aprs::Info aprs;                    // aprs::parse() of aprsInfo (type "" when it did not parse)
    int batteryPct = -1;                // MeshCom position "/B=" (battery, percent)
    std::string raw;                    // the payload as printable text (other bytes as \xNN)
};

// Thread safety: decode* are const and may run while add* is called from another thread (the class locks internally).
class MeshProto {
public:
    MeshProto();                        // knows the published default keys (Meshtastic default channel, MeshCore Public)
    ~MeshProto();
    bool addMeshtasticChannel(const std::string& name, const std::string& base64Psk);      // false: bad key
    bool addMeshCoreChannel(const std::string& name, const std::string& secret);           // hex or base64; false: bad key
    MeshDecodeResult decodeMeshtastic(const uint8_t* payload, size_t n, const MeshRadioInfo& radio) const;
    MeshDecodeResult decodeMeshCore(const uint8_t* payload, size_t n, const MeshRadioInfo& radio) const;
    // added later
    void clearUserChannels();                                                              // drops the added channels, keeps the defaults
    std::vector<std::string> channelNames(MeshProtocol p) const;                           // known channels, defaults first
private:
    struct State;
    std::unique_ptr<State> s_;
};

// LoRa settings of the two protocols
struct MeshLoraSettings {
    std::string name;                   // preset name
    double freqHz = 0;                  // 0 = depends on the channel (Meshtastic slot)
    int sf = 11;
    double bwHz = 250000;
    int cr = 5;                         // 4/5
    int preamble = 16;
    uint8_t syncWord = 0x2B;
    bool ldro = false;                  // low data rate optimisation
};
bool meshtasticPreset(const std::string& preset, MeshLoraSettings& out);                 // "LongFast", "MediumFast", ...; false if unknown
double meshtasticSlotHz(const std::string& region, const std::string& preset);          // region "EU_868", "US", ...; 0 = unknown
MeshLoraSettings meshcoreDefaults(const std::string& region);                            // region "EU", "US"

// Packet builders for the test signal: the bytes the firmware puts on air (header + encrypted payload, without the LoRa CRC)
struct MeshtasticNode {
    uint32_t num = 0;
    std::string longName, shortName;
    int hwModel = 0;                    // mesh.proto HardwareModel value
    double lat = 0, lon = 0, altM = 0;
    int role = 0;                       // Config.DeviceConfig.Role value (0 CLIENT)
};
std::vector<uint8_t> meshtasticText(const MeshtasticNode& from, uint32_t to, uint32_t packetId, int hopLimit, int hopStart, const std::string& text);
std::vector<uint8_t> meshtasticNodeInfo(const MeshtasticNode& from, uint32_t packetId, int hopLimit, int hopStart);
std::vector<uint8_t> meshtasticPosition(const MeshtasticNode& from, uint32_t packetId, int hopLimit, int hopStart, uint32_t unixTime);
std::vector<uint8_t> meshtasticTelemetry(const MeshtasticNode& from, uint32_t packetId, int hopLimit, int hopStart, double batteryPct, double voltage,
                                         double channelUtil, double airUtilTx, uint32_t uptimeS);
struct MeshCoreNode {
    uint8_t seed = 1;                   // the node's (test) key pair is derived from this
    std::string name;
    double lat = 0, lon = 0;
    bool repeater = false;
};
std::vector<uint8_t> meshcoreAdvert(const MeshCoreNode& n, uint32_t unixTime);
std::vector<uint8_t> meshcorePublicText(const MeshCoreNode& from, uint32_t unixTime, const std::string& text, const std::vector<uint8_t>& path);

// ---- added later: more builders and helpers (the ones above are unchanged) ----
// Generic Meshtastic packet: header + AES-CTR(key, nonce) of the Data protobuf. key empty = not encrypted, 16 or 32 bytes.
std::vector<uint8_t> meshtasticRaw(uint32_t from, uint32_t to, uint32_t packetId, int hopLimit, int hopStart, bool wantAck,
                                   const std::string& channelName, const std::vector<uint8_t>& key, const std::vector<uint8_t>& dataProto);
std::vector<uint8_t> meshtasticData(int portnum, const std::vector<uint8_t>& payload, bool wantResponse = false, uint32_t dest = 0,
                                    uint32_t source = 0, uint32_t requestId = 0, uint32_t replyId = 0, uint32_t emoji = 0);   // Data protobuf
std::vector<uint8_t> meshtasticTextEx(const MeshtasticNode& from, uint32_t to, uint32_t packetId, int hopLimit, int hopStart, const std::string& text,
                                      uint32_t replyId, bool wantAck);
std::vector<uint8_t> meshtasticPositionEx(const MeshtasticNode& from, uint32_t packetId, int hopLimit, int hopStart, uint32_t unixTime, int precisionBits);
std::vector<uint8_t> meshtasticEnvTelemetry(const MeshtasticNode& from, uint32_t packetId, int hopLimit, int hopStart, double tempC, double humidity, double pressureHpa);
std::vector<uint8_t> meshtasticRoutingAck(const MeshtasticNode& from, uint32_t to, uint32_t packetId, int hopLimit, int hopStart, uint32_t requestId, int errorReason);
// route lists are node numbers; snr values are dB * 4 as the firmware stores them
std::vector<uint8_t> meshtasticTraceroute(const MeshtasticNode& from, uint32_t to, uint32_t packetId, int hopLimit, int hopStart, bool reply, uint32_t requestId,
                                          const std::vector<uint32_t>& route, const std::vector<int>& snrTowards,
                                          const std::vector<uint32_t>& routeBack, const std::vector<int>& snrBack);
struct MeshNeighbor { uint32_t node = 0; float snr = 0; };
std::vector<uint8_t> meshtasticNeighborInfo(const MeshtasticNode& from, uint32_t packetId, int hopLimit, int hopStart, uint32_t intervalS, const std::vector<MeshNeighbor>& n);

std::string meshtasticNodeIdString(uint32_t num);                        // "!1a2b3c4d"
std::string meshtasticHwModelName(int hw);                               // "TBEAM"; "HW_<n>" if unknown
int meshtasticHwModelValue(const std::string& name);                     // -1 if unknown
uint8_t meshtasticChannelHash(const std::string& name, const std::vector<uint8_t>& key);   // xor of the name bytes and of the key bytes
bool meshtasticExpandPsk(const std::vector<uint8_t>& psk, std::vector<uint8_t>& key);      // firmware rules for short keys; false: bad length
bool meshLoraLdro(int sf, double bwHz);                                  // low data rate optimisation as RadioLib decides (symbol time >= 16 ms)

// MeshCore
bool meshcoreNodeKeys(const MeshCoreNode& n, uint8_t seed[32], uint8_t pub[32]);                  // test key pair derived from the node's seed
std::vector<uint8_t> meshcoreGroupText(const uint8_t secret16[16], const MeshCoreNode& from, uint32_t unixTime, const std::string& text,
                                       const std::vector<uint8_t>& path);                         // any 16 byte channel secret
std::vector<uint8_t> meshcoreAck(uint32_t crc, const std::vector<uint8_t>& path);                 // flood ACK
const uint8_t* meshcorePublicSecret();                                                            // the 16 bytes of the Public channel

// ---- LoRa APRS (lora-aprs / CA2RXU iGate and tracker firmware) and MeshCom 4 (icssw.org) ----
// LoRa APRS: payload = "<" 0xFF 0x01 + a TNC2 text packet "SRC>DEST,PATH:info" (lora_utils.cpp of richonguzman/LoRa_APRS_iGate,
// encodeLoRaAPRS in MeshCom's aprs_functions.cpp). A trailing NUL, CR or LF is dropped.
// MeshCom: type byte ':' text, '!' position, '@' HEY, 'A' (0x41) ack; message id (4 bytes, little endian); flags/max hop byte;
// "SRCPATH>DEST" + type + payload + 0x00; hardware id; modulation (low nibble) | country << 4; 16-bit sum of all bytes so far (big
// endian); firmware version; last hardware; sub version; 0x7E (decodeAPRS / encodeAPRS in icssw-org/MeshCom-Firmware
// src/aprs_functions.cpp, MIT licence). An ack is 0x41, its own id (4), flags, the acknowledged id (4) and two more bytes (handleACK in
// lora_functions.cpp, ack_functions.h: byte 5 = 0x80 | hops, bytes 10-11 = 01 00).
MeshDecodeResult meshDecodeLoraAprs(const uint8_t* payload, size_t n);
MeshDecodeResult meshDecodeMeshCom(const uint8_t* payload, size_t n);
std::string meshPrintable(const uint8_t* p, size_t n);                    // printable ASCII kept, other bytes as \xNN

// LoRa settings of the two (preamble: RadioLib's default 8 for LoRa APRS; MeshCom LORA_PREAMBLE_LENGTH 32, 8 in the country profiles)
std::vector<MeshLoraSettings> loraAprsPresets();      // "LoRa APRS EU" 433.775, "LoRa APRS PL" 434.855 (SF9 4/7), "LoRa APRS UK" 439.9125, "LoRa APRS 915"
std::vector<MeshLoraSettings> meshcomPresets();       // "MeshCom EU" 433.175 SF11 250 kHz 4/6, UK, LA, 868, 915, VR2, 435, 436, 442 (country_profile.cpp)

// builders for the test signal
std::vector<uint8_t> loraAprsFrame(const std::string& tnc2);
struct MeshComFrame {
    char type = ':';                    // ':' text, '!' position, '@' HEY
    uint32_t msgId = 0;
    int maxHop = 5;                     // 0..15
    bool server = false, track = false, appOffline = false, mesh = true;
    std::string sourcePath;             // "A61MC-2" or, relayed, "A61MC-2,A61MC-1"
    std::string dest = "*";             // "*" all, a call or a group number
    std::string payload;                // after the type character
    uint8_t hw = 4, mod = 3, fw = 35, lastHw = 0x84;
    char subVersion = 'a';
};
std::vector<uint8_t> meshcomBuild(const MeshComFrame& f);

} // namespace dect2
