// A small MQTT 3.1.1 client and the packet encoder/decoder under it (QoS 0 and 1, retain, user/password, last will, TLS through core/src/net/tls_conn).
// Nothing here connects by itself: the Hub (integrations.h) opens a connection only to a broker the user typed and switched on. The decoder side
// (decodeConnect, decodePublish) exists for the tests' fake broker.
#pragma once
#include <cstdint>
#include <string>

namespace dect2::integrations::mqtt {

enum PacketType : uint8_t { CONNECT = 1, CONNACK = 2, PUBLISH = 3, PUBACK = 4, SUBSCRIBE = 8, SUBACK = 9, PINGREQ = 12, PINGRESP = 13, DISCONNECT = 14 };

struct Packet {
    uint8_t type = 0;
    uint8_t flags = 0;       // the low nibble of the first byte
    std::string body;        // after the fixed header
};

struct ConnectOpts {
    std::string clientId;
    std::string user, password;       // empty user: none sent
    uint16_t keepAlive = 60;
    bool cleanSession = true;
    std::string willTopic, willPayload;   // empty topic: no will
    bool willRetain = false;
};

struct Publish {
    std::string topic, payload;
    int qos = 0;
    bool retain = false, dup = false;
    uint16_t pid = 0;                 // QoS 1 only
};

// ---- the wire ----
void putVarInt(std::string& out, uint32_t v);                       // remaining length, 1 to 4 bytes (v up to 268435455)
int getVarInt(const uint8_t* p, size_t n, uint32_t& v);             // bytes used; 0 = need more bytes; -1 = malformed
// One packet from the front of buf: bytes consumed, 0 = incomplete, -1 = malformed (more than 4 length bytes, bad type 0/15).
int parsePacket(const std::string& buf, Packet& out);

std::string encodeConnect(const ConnectOpts& o);
bool decodeConnect(const std::string& body, ConnectOpts& o);
std::string encodeConnack(bool sessionPresent, uint8_t returnCode);
bool decodeConnack(const std::string& body, bool& sessionPresent, uint8_t& returnCode);
std::string encodePublish(const Publish& p);
bool decodePublish(const Packet& pk, Publish& p);
std::string encodePuback(uint16_t pid);
bool decodePuback(const std::string& body, uint16_t& pid);
std::string encodePingreq();
std::string encodePingresp();
std::string encodeDisconnect();
const char* connackText(uint8_t rc);

// A topic name to publish to: not empty, at most 65535 bytes, no wildcards (+ #), no NUL.
bool topicNameValid(const std::string& t);

// ---- the client ----
struct ClientConfig {
    std::string host;
    int port = 1883;
    bool tls = false;                 // refused (no plain fallback) when this build has no TLS
    ConnectOpts connect;
    int connectTimeoutMs = 5000;
    int ackTimeoutMs = 5000;
};

class Client {
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    void configure(const ClientConfig& c);
    bool connect(std::string& err);                    // TCP (or TLS) + CONNECT + CONNACK; err never holds the password
    bool connected() const;
    // QoS 0: written to the socket. QoS 1: waits for the PUBACK (ackTimeoutMs). false and err on any failure (the connection is then closed).
    bool publish(const std::string& topic, const std::string& payload, int qos, bool retain, std::string& err);
    bool ping(std::string& err);                       // PINGREQ and waits for the PINGRESP
    void disconnect();                                 // DISCONNECT, then closes
    double idleSeconds() const;                        // since the last packet was written
private:
    struct Impl;
    Impl* p_;
};

}  // namespace dect2::integrations::mqtt
