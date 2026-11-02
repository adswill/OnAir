// OnAir Web relay client: makes a server reachable from the internet with no port forwarding, no router settings and no third-party program.
// The server opens ONE outbound WebSocket to a relay service (default wss://relay.on-air.dev/relay/v1/host), proves who it is with its Ed25519 key
// (the same key file as the directory listing) and gets a public https address back. Every listener that opens that address is carried to this
// server as a virtual connection inside the one WebSocket: the relay says "open connection 7 (from address A)", the bytes of the listener's HTTP /
// WebSocket stream travel in both directions, and this client connects each virtual connection to the web server on 127.0.0.1. The web server needs no
// change: it sees an ordinary local client plus an X-Forwarded-For header (web_trust_proxy), so the per-address limits, bans and kicks keep working.
// The relay is blind to nothing the browser sends (it terminates TLS for its own domain), so it is a service the host chooses to trust; the host
// can switch it off at any time and the address stops answering. The wire format is in docs/ONAIR-WEB-RELAY.md. Pure parts (url, signing text, frame
// format, header rewrite) are tested without a network (tests/server/test_web_relay.cpp); the client is tested against a fake relay on loopback.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace onair_web {

struct RelayConfig {
    bool on = false;                  // web_relay
    std::string url = "wss://relay.on-air.dev/relay/v1/host";   // web_relay_url (wss, or ws for this computer only: the tests)
    int localPort = 8073;             // the web server's port on 127.0.0.1 (set by the server)
    std::string keyFile;              // web_directory_key: the Ed25519 seed (made on first start, shared with the directory listing)
    std::string name;                 // shown to the relay in its logs (the server's name); not needed for the address
    std::string version;              // software version
    int maxConns = 64;                // virtual connections at once (the server sets 2 * max users + 8)
    std::string statusFile;           // where the status JSON is kept (empty: not written)
    long long nowOverride = 0;        // tests: the clock
};

struct RelayStatus {
    std::string state = "off";        // off, connecting, online, retrying, failed
    std::string url;                  // the public address (when online)
    std::string code, message, advice;   // the last problem, in plain words
    int conns = 0;                    // virtual connections right now
    long long bytesIn = 0, bytesOut = 0, connsTotal = 0, retryIn = 0, onlineSince = 0;
};

// ---- pure parts
struct RelayUrl { bool tls = true; std::string host, path; int port = 443; };
bool relayParseUrl(const std::string& in, RelayUrl& out, std::string& err);   // wss://host[:port]/path ; ws:// only for 127.0.0.1 / localhost
// The text that is signed: "OnAir-Relay-v1\nid=..\npub=..\nts=..\nnonce=..\n" (sig = Ed25519 over it, 128 hex characters)
std::string relayCanonical(const std::string& id, const std::string& pub, long long ts, const std::string& nonce);
std::string relayHelloJson(const RelayConfig& c, const std::string& id, const std::string& pub, long long ts, const std::string& nonce, const std::string& sigHex);
// Binary frames inside the WebSocket: [type:1][connection id:4 big endian][payload]
enum RelayFrame : uint8_t { kRelayOpen = 1, kRelayData = 2, kRelayClose = 3 };
std::string relayFrame(uint8_t type, uint32_t id, const void* payload, size_t n);
bool relayParseFrame(const unsigned char* p, size_t n, uint8_t& type, uint32_t& id, const unsigned char*& payload, size_t& plen);
// The request header of a listener's first request as the web server should see it: any X-Forwarded-For / X-Real-IP / Forwarded the listener
// sent is dropped, "X-Forwarded-For: <ip>" is added, and for a plain request (not an upgrade) "Connection: close" is forced so a later request on the
// same connection can never carry a header this function did not look at. `head` ends with the empty line.
std::string relayRewriteHead(const std::string& head, const std::string& ip);
// What to tell the owner for a problem class ("dns", "unreachable", "tls", "no_tls", "clock", "rejected", "busy", "banned", ...)
std::string relayAdvice(const std::string& code);
std::string relayStatusJson(const RelayStatus& s);
bool relayParseStatus(const std::string& json, RelayStatus& out);

class RelayClient {
public:
    virtual ~RelayClient() = default;
    // Loads or makes the key, checks the address and starts the link thread. False (err says why): bad address, no key file.
    virtual bool start(const RelayConfig& c, std::string& err) = 0;
    virtual void stop() = 0;
    virtual RelayStatus status() const = 0;
};
RelayClient* makeRelayClient();

} // namespace onair_web
