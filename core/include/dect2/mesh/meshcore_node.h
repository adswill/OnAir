// MeshCore node: OnAir as a MeshCore companion device that sends and receives. Packet formats, routing and crypto follow the MeshCore firmware
// (meshcore-dev/MeshCore, MIT licence): src/Packet.{h,cpp} (header byte, transport codes, path length byte with the hash size in its top two
// bits), src/Mesh.cpp (onRecvPacket, routeRecvPacket, createAdvert / createDatagram / createGroupDatagram / createPathReturn / createAck,
// sendFlood / sendDirect), src/Identity.cpp + lib/ed25519 (orlp: 64 byte private key, ed25519_key_exchange), src/Utils.cpp (AES-128-ECB with
// zero padding, 2 byte HMAC-SHA256 MAC keyed with the 32 byte secret), src/helpers/BaseChatMesh.cpp (text message layout, the ACK hash,
// path return with the ACK, adverts to contacts, group text "name: text"), src/helpers/SimpleMeshTables.h (the table of packets seen),
// src/helpers/AdvertDataHelpers.cpp (advert app data), examples/companion_radio/MyMesh.cpp (send timeouts). The code here is written for
// OnAir; nothing of the firmware is copied.
//
// What it does:
//   - an identity made on this computer (random seed -> orlp 64 byte private key), kept by the app in its own folder, never sent anywhere;
//     import and export as the 64 byte private key in hex (the MeshCore apps' export)
//   - adverts (flood or zero hop) with the name and an optional position, signed; adverts heard fill the contact list (signature checked,
//     replayed timestamps refused)
//   - direct text messages to a contact, encrypted with the shared secret, with the ACK the firmware expects and retries: attempt 0..2 (the
//     attempt number is in the message, so each attempt has its own ACK); the last one goes out as a flood with the learned path dropped
//   - channel messages on the Public channel, on "#name" hashtag channels (key = first 16 bytes of SHA-256 of the name) and on channels the
//     user adds with a 16 or 32 byte key
//   - path learning: a flood message to us is answered by a PATH packet that carries the ACK; a PATH packet to us sets the contact's route
//     and is answered by a PATH back (as Mesh.cpp does); messages to a contact with a route go direct along it
//   - repeater mode (off by default): flood packets get this node's hash appended and are sent again after a random delay, direct packets
//     whose next hop is this node are passed on; a table of packet hashes stops loops
//   - a send queue with due times (the firmware's ACK delay, retransmit delays), the airtime of every frame and the duty-cycle budget
// Nothing is refused here for regulatory reasons: the duty-cycle numbers are for the user to see (dutyCycleRule), the app warns.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace dect2 {
namespace meshcore {

constexpr size_t kMaxPayload = 184;   // MAX_PACKET_PAYLOAD
constexpr size_t kMaxPath = 64;       // MAX_PATH_SIZE (bytes)
constexpr size_t kMaxText = 160;      // MAX_TEXT_LEN (10 cipher blocks)
constexpr size_t kMaxAdvertData = 32; // MAX_ADVERT_DATA_SIZE

enum Route { RouteTransportFlood = 0, RouteFlood = 1, RouteDirect = 2, RouteTransportDirect = 3 };
enum Type { TReq = 0, TResponse = 1, TTxtMsg = 2, TAck = 3, TAdvert = 4, TGrpTxt = 5, TGrpData = 6, TAnonReq = 7, TPath = 8, TTrace = 9,
            TMultipart = 10, TControl = 11, TRawCustom = 15 };
enum AdvType { AdvNone = 0, AdvChat = 1, AdvRepeater = 2, AdvRoom = 3, AdvSensor = 4 };

// One packet as it is in the LoRa payload (Packet::writeTo / readFrom)
struct Packet {
    uint8_t header = 0;
    uint16_t transport[2] = {0, 0};
    uint8_t pathLen = 0;                 // the encoded byte: hash count in bits 0..5, hash size - 1 in bits 6..7
    std::vector<uint8_t> path;           // hashCount() * hashSize() bytes
    std::vector<uint8_t> payload;
    float snrDb = 0;                     // when received

    int route() const { return header & 3; }
    int type() const { return (header >> 2) & 15; }
    int version() const { return (header >> 6) & 3; }
    bool flood() const { return route() == RouteFlood || route() == RouteTransportFlood; }
    bool direct() const { return route() == RouteDirect || route() == RouteTransportDirect; }
    bool hasTransport() const { return route() == RouteTransportFlood || route() == RouteTransportDirect; }
    int hashSize() const { return (pathLen >> 6) + 1; }
    int hashCount() const { return pathLen & 63; }
    void setRoute(int r) { header = (uint8_t)((header & ~3) | (r & 3)); }
    std::vector<uint8_t> bytes() const;
    void packetHash(uint8_t out[8]) const;   // calculatePacketHash: SHA-256 of the type (and the path length byte for TRACE) and the payload
};
bool parsePacket(const uint8_t* b, size_t n, Packet& out);   // false: not a valid encoding (Packet::readFrom rules)
bool validPathLen(uint8_t pathLen);

// ---- identity ----
struct Identity {
    uint8_t prv[64] = {};                // orlp private key: clamped scalar + signing prefix
    uint8_t pub[32] = {};
    bool valid = false;
};
// fills `seed` with random bytes from the system (std::random_device)
void systemRandom(uint8_t* p, size_t n);
// From a 32 byte seed (ed25519_create_keypair). False when the public key starts with 0x00 or 0xFF (the firmware refuses those).
bool identityFromSeed(const uint8_t seed[32], Identity& out);
// A new identity from system randomness (a new seed until the public key is acceptable)
Identity identityNew();
// The MeshCore apps' private key export: 64 bytes in hex (optionally followed by the 32 byte public key). The key is checked as
// LocalIdentity::validatePrivateKey does (prefix, key exchange with the firmware's test key both ways). False: not such a key.
bool identityFromPrivateHex(const std::string& hex, Identity& out, std::string* why = nullptr);
std::string identityPrivateHex(const Identity& id);
std::string hexOf(const uint8_t* p, size_t n);
// the shared secret of two identities (LocalIdentity::calcSharedSecret)
void sharedSecret(const Identity& self, const uint8_t otherPub[32], uint8_t out[32]);

// ---- channels ----
struct Channel {
    std::string name;
    uint8_t secret[32] = {};             // 16 byte keys are zero padded (the AES key is always the first 16 bytes, the MAC key all 32)
    int keyLen = 16;
    uint8_t hash = 0;                    // first byte of SHA-256 of the key (keyLen bytes)
};
Channel publicChannel();                 // "Public": 8b3387e9c5cdea6ac9e5edbaa115cd72
// "#name": a hashtag channel (key = SHA-256("#name")[0..16]); otherwise key is 32 or 64 hex digits or base64 of 16 or 32 bytes
bool makeChannel(const std::string& name, const std::string& key, Channel& out, std::string* why = nullptr);
std::string channelKeyHex(const Channel& c);

// ---- advert app data ----
struct AdvertData {
    int type = AdvChat;
    bool hasPos = false;
    double lat = 0, lon = 0;             // degrees (sent as int32 micro-degrees)
    std::string name;
    uint16_t feat1 = 0, feat2 = 0;
};
std::vector<uint8_t> encodeAdvertData(const AdvertData& a);   // AdvertDataBuilder::encodeTo (the name cut to fit 32 bytes on a UTF-8 boundary)
bool parseAdvertData(const uint8_t* p, size_t n, AdvertData& out);

// ---- packet builders (route, path: set by the caller through floodOf / directOf) ----
Packet buildAdvert(const Identity& id, uint32_t unixTime, const AdvertData& app);
// TXT_MSG: timestamp, attempt (0..3 in the flags byte; above 3 it is also put after the text's terminator, as composeMsgPacket does), text.
// expectedAck: the 4 byte ACK the recipient sends back (SHA-256 of the plain data and the sender's public key).
bool buildText(const Identity& self, const uint8_t destPub[32], const uint8_t secret[32], uint32_t unixTime, int attempt, const std::string& text,
               Packet& out, uint32_t& expectedAck);
Packet buildGroupText(const Channel& c, uint32_t unixTime, const std::string& senderName, const std::string& text);
Packet buildAck(const uint8_t* ack, size_t n);
// PATH: the path (encoded length byte + bytes) and an extra (type + bytes), or the firmware's dummy extra (0xFF + 4 random bytes) when none
Packet buildPathReturn(uint8_t destHash, uint8_t srcHash, const uint8_t secret[32], uint8_t pathLen, const std::vector<uint8_t>& path,
                       int extraType, const std::vector<uint8_t>& extra, const uint8_t rnd4[4]);
void floodOf(Packet& p, int hashSize = 1);                       // flood, empty path of that hash size
void directOf(Packet& p, uint8_t pathLen, const std::vector<uint8_t>& path);
// encrypt-then-MAC (Utils::encryptThenMAC) and its check (Utils::MACThenDecrypt); 0 / false on a bad MAC or length
std::vector<uint8_t> encryptThenMac(const uint8_t secret[32], const uint8_t* data, size_t n);
bool macThenDecrypt(const uint8_t secret[32], const uint8_t* src, size_t n, std::vector<uint8_t>& plain);

// ---- LoRa airtime and duty cycle ----
struct Radio {
    double freqHz = 869.618e6;
    double bwHz = 62500;
    int sf = 8, cr = 8;                  // cr 5..8 = 4/5..4/8
    int preamble = 32;
    bool crc = true;
    int pathHashSize = 1;                // the hash size of the floods this node starts (a region's "2B" preset: 2)
};
double airtimeSec(const Radio& r, size_t payloadBytes);   // Semtech SX127x/SX126x time on air (explicit header)
struct DutyRule { std::string band; double limit = 0; };  // limit: fraction of time (0.01 = 1 %); 0: no limit in the table
// The European short-range device sub-bands of ERC Recommendation 70-03 annex 1 (863 - 870 MHz) and 433.05 - 434.79 MHz; elsewhere none.
DutyRule dutyCycleRule(double freqHz);
// airtime used in a sliding window (one hour by default)
class DutyMeter {
public:
    void add(double now, double sec) { log_.push_back({now, sec}); trim(now); }
    double used(double now, double window = 3600) { trim(now, window); double s = 0; for (const auto& e : log_) if (e.first >= now - window) s += e.second; return s; }
    void clear() { log_.clear(); }
private:
    void trim(double now, double window = 3600) { while (!log_.empty() && log_.front().first < now - window) log_.pop_front(); }
    std::deque<std::pair<double, double>> log_;
};

// ---- the node ----
struct Contact {
    uint8_t pub[32] = {};
    std::string name;
    int type = AdvChat;
    bool hasPos = false;
    double lat = 0, lon = 0;
    uint32_t lastAdvert = 0;             // the sender's advert timestamp (replay check)
    double heardAt = 0;                  // unix time on this computer
    int hops = -1;                       // path hash count of the last advert heard
    float snrDb = 0;
    int outPathLen = -1;                 // -1: no route known (send flood); else the encoded path length byte
    std::vector<uint8_t> outPath;
    std::string pubHex() const { return hexOf(pub, 32); }
};

struct Message {
    enum Kind { Direct = 0, Group = 1 };
    enum Status { Received = 0, Queued = 1, Sent = 2, Delivered = 3, Failed = 4 };
    int kind = Direct;
    std::string peer;                    // Direct: the contact's public key hex; Group: the channel name
    std::string from;                    // sender name ("" = us)
    std::string text;
    uint32_t senderTime = 0;
    double at = 0;                       // unix time on this computer
    bool outgoing = false;
    int status = Received;
    int attempts = 0;
    int hops = -1;                       // received: path hash count; flood -1 when direct
    float snrDb = 0;
    uint32_t id = 0;                     // a number of its own (outgoing: retries keep it)
    double rtt = 0;                      // delivered: seconds from the last attempt to the ACK
};

struct Outgoing {
    std::vector<uint8_t> bytes;
    double due = 0;                      // monotonic seconds
    int priority = 0;                    // lower first (the firmware's priorities)
    std::string what;                    // "advert", "message to Bob (attempt 1)", "ACK", "path", "repeat"
};

struct NodeConfig {
    std::string name = "OnAir";
    bool sharePosition = false;
    double lat = 0, lon = 0;
    bool repeater = false;               // pass on packets (off by default)
    Radio radio;
    double turnaroundSec = 1.5;          // added to every ACK timeout: the receiver is off while a half-duplex radio sends
    int maxAttempts = 3;
    size_t maxMessages = 500;
    size_t maxContacts = 300;
};

class Node {
public:
    Node();
    // the clock (unix seconds) and the random source; tests set them for repeatable packets
    std::function<uint32_t()> unixClock;
    std::function<void(uint8_t*, size_t)> random;
    std::function<void(const std::string&)> log;

    void setIdentity(const Identity& id);
    const Identity& identity() const { return id_; }
    void setConfig(const NodeConfig& c) { cfg_ = c; }
    const NodeConfig& config() const { return cfg_; }
    NodeConfig& config() { return cfg_; }

    // a frame from the radio (CRC good); now = monotonic seconds
    void receive(const uint8_t* b, size_t n, float snrDb, double now);
    // user actions; false + why when nothing could be built (no identity, unknown contact, text too long is cut)
    bool sendAdvert(double now, bool flood = true);
    bool sendText(const std::string& contactPubHex, const std::string& text, double now, std::string* why = nullptr);
    bool sendChannel(const std::string& channelName, const std::string& text, double now, std::string* why = nullptr);
    void resetPath(const std::string& contactPubHex);
    bool removeContact(const std::string& contactPubHex);
    bool addChannel(const Channel& c);   // false: the name is taken
    bool removeChannel(const std::string& name);   // the Public channel stays
    // retries and timeouts; call often
    void tick(double now);
    // the next frame whose time has come (lowest priority number, then the earliest); false: none due
    bool popDue(double now, Outgoing& out);
    double nextDue() const;              // monotonic time of the next queued frame (-1: none)
    size_t queued() const { return out_.size(); }
    // tell the node a frame went out (airtime for the duty meter)
    void noteSent(const Outgoing& o, double now);

    const std::vector<Contact>& contacts() const { return contacts_; }
    const std::vector<Channel>& channels() const { return channels_; }
    const std::deque<Message>& messages() const { return msgs_; }
    void clearMessages() { msgs_.clear(); }
    DutyMeter& duty() { return duty_; }
    uint64_t seenCount() const { return seenTotal_; }
    uint64_t repeated() const { return repeated_; }

    // what the app keeps on disk: identity, name, contacts with their routes, channels, recent messages. Plain text, one record per line.
    std::string save() const;
    bool load(const std::string& s);

private:
    struct PendingText {
        uint32_t id = 0;
        std::string pubHex, text;
        uint32_t ts = 0;
        int attempt = 0;
        double deadline = 0;
        double sentAt = 0;
        std::vector<uint32_t> acks;      // every attempt's expected ACK
        bool flood = false;
    };
    Contact* findContact(const uint8_t pub[32]);
    Contact* findContactHex(const std::string& hex);
    Message* findMsg(uint32_t id);
    bool wasSeen(const Packet& p) const;
    void markSeen(const Packet& p);
    void queue(const Packet& p, double due, int prio, const std::string& what);
    void addMsg(Message m);
    void onAdvert(Packet& p, double now);
    void onPeer(Packet& p, double now);
    void onGroup(Packet& p, double now);
    void onAckValue(const uint8_t* ack, double now);
    void routeFlood(Packet& p, double now);
    bool attemptText(PendingText& t, double now);
    double frameAir(const Packet& p) const;
    uint32_t uniqueTime();
    uint8_t selfHash() const { return id_.pub[0]; }

    Identity id_;
    NodeConfig cfg_;
    std::vector<Contact> contacts_;
    std::vector<Channel> channels_;
    std::deque<Message> msgs_;
    std::vector<PendingText> pend_;
    std::vector<Outgoing> out_;
    uint8_t seen_[160][8] = {};
    int seenNext_ = 0;
    uint64_t seenTotal_ = 0, repeated_ = 0;
    uint32_t lastUnique_ = 0, nextId_ = 1;
    DutyMeter duty_;
};

} // namespace meshcore
} // namespace dect2
