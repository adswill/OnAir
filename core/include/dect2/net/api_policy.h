// The pure rules of the remote-control API: token check, rate limit, request size limits, the route table (with the refusal list), the
// decoded-message line format (WebSocket, polling and UDP), the ring that holds the recent messages and the rigctl (Hamlib rigctld subset) line
// parser. No sockets here: remote_api.cpp, rigctl_server.cpp and api_udp.cpp use these, tests/net/test_api_policy.cpp tests them.
// The API can transmit (3.2: the operator decides, the response carries the warnings), but never reads or writes voice keys or credentials, or changes security settings: parseRoute() refuses
// such a path whatever the host program offers, and the route table below is the whole list of what exists.
#pragma once
#include "dect2/automation/event.h"
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dect2::apipol {

constexpr int kApiVersion = 1;
constexpr size_t kMaxHeaderBytes = 8192, kMaxBodyBytes = 4096, kMaxPathBytes = 512, kMinTokenLen = 16, kMaxUdpLine = 1400, kMaxRigctlLine = 256;

// ---- token
bool tokenUsable(const std::string& t);                       // >= 16 printable characters without spaces
bool tokenEqual(const std::string& a, const std::string& b);  // the time does not depend on where they differ (nor on the lengths up to 256)
// The token a request carries: "Authorization: Bearer <t>" first, else ?token=<t> in the query string. Empty: none.
std::string extractToken(const std::string& authorizationHeader, const std::string& query);
enum class Auth { Allow, NeedToken, NoTokenConfigured };
// loopback: the peer is 127.x. requireLocal: a loopback peer needs the token too. A non-loopback peer always needs it; with no usable token it is refused.
Auth authorise(bool loopback, bool requireLocal, const std::string& expected, const std::string& provided);

// ---- rate limit: a token bucket per client key (an address). allow() false: answer 429. Time is passed in (seconds) so tests need no clock.
class RateLimiter {
public:
    RateLimiter(double perSecond = 20, double burst = 40, size_t maxKeys = 256) : rate_(perSecond), burst_(burst), maxKeys_(maxKeys) {}
    bool allow(const std::string& key, double now);
    double retryAfter(const std::string& key, double now) const;   // seconds until one more request is allowed
    size_t keys() const { return b_.size(); }
private:
    struct B { double tokens = 0, t = 0; };
    double rate_, burst_;
    size_t maxKeys_;
    std::map<std::string, B> b_;
};

// ---- sizes
enum class Size { Ok, HeaderTooLarge, BodyTooLarge, BadLength, PathTooLong };
Size checkSizes(size_t headerBytes, const std::string& contentLengthHeader, size_t pathBytes);

// ---- routes
enum class Ep {
    None, Page, Index, Status, Devices, Modes, Telemetry, Spectrum, Rotator, Ws, Events, Memories, Sessions, Scheduler, Audio, Recording,
    Start, Stop, Mode, Frequency, SampleRate, Gain, AutoGain, Bandwidth, Squelch, Volume, Mute, RecordStart, RecordStop,
    RotatorGoto, RotatorStop, RotatorPark, TxStatus, TxStart, TxStop, Extra
};
struct Route {
    Ep ep = Ep::None;
    int status = 404;            // 200: found; 404 no such endpoint; 403 refused by the refusal list; 400 malformed path; 404 also for an API version this build does not have
    bool post = false;           // the endpoint takes POST (else GET)
    std::string allow;           // "GET" or "POST" (Allow header on a 405)
    std::string msg;
};
// path without the query string. Normalisation is NOT done: a path with "..", "//", "%", a control character or a backslash is malformed (400).
Route parseRoute(const std::string& method, const std::string& path);
// words that make a path refused: key material, credentials, security settings (checked on every path segment); transmit is NOT among them
bool pathRefused(const std::string& path);
std::vector<std::string> endpointList();   // "GET /api/v1/status", ... (for GET /api/v1 and docs/REMOTE-API.md)

// ---- the decoded-message line: {"v":1,"type":..,"time":..,"mode":..,"freqHz":..,"fields":{..}} (one JSON object, no newline inside)
std::string eventLine(const automation::Event& e, uint64_t seq = 0, size_t maxBytes = 0);   // maxBytes > 0 and the line is longer: the fields are left out and "truncated":true is added
std::string jsonString(const std::string& s);   // with quotes

// The recent lines, bounded; each has a number that rises by one. Thread safe.
class EventRing {
public:
    explicit EventRing(size_t cap = 512) : cap_(cap) {}
    uint64_t push(const std::string& line);                        // returns its number
    // cursor == UINT64_MAX: "from now" (sets the cursor to the next number, returns nothing). Else the lines with a number >= cursor, at most maxN; cursor moves past them.
    // gap is set when lines the client has not seen were dropped from the ring.
    void since(uint64_t& cursor, size_t maxN, std::vector<std::string>& out, bool* gap = nullptr) const;
    uint64_t next() const;
private:
    mutable std::mutex mu_;
    size_t cap_;
    uint64_t first_ = 0;   // number of front()
    std::deque<std::string> q_;
};

// ---- rigctl (Hamlib rigctld subset: f F m M t T s S l q, plus v V and \chk_vfo)
class RigctlRadio {
public:
    virtual ~RigctlRadio() = default;
    virtual bool getFreq(double& hz) = 0;
    virtual bool setFreq(double hz) = 0;
    virtual bool getMode(std::string& mode, int& passbandHz) = 0;     // "AM", "FM", "WFM", "USB", "LSB", "CW" ...
    virtual bool setMode(const std::string& mode, int passbandHz) = 0;
    virtual bool getStrengthDb(double& db) { (void)db; return false; }
    virtual bool getPtt(bool& on) { on = false; return false; }   // true: the app is transmitting
    virtual bool setPtt(bool on) { (void)on; return false; }      // on: start the transmission set up in the app; off: stop it
};
// One line in, the reply out (already with its newlines). quit is set by q / Q. T 1 starts the transmission the host has set up (RPRT -11 when it cannot), T 0 stops it, t answers 1 or 0.
// Anything unknown: "RPRT -4"; a bad argument: "RPRT -1"; a line over 256 bytes: "RPRT -8".
std::string rigctlHandle(const std::string& line, RigctlRadio& radio, bool& quit);
// The per-connection state. required: the commands that change the radio (F M S T) need a prior "\auth <token>" on this connection (the reply
// is "RPRT 0"); with an empty token nothing can authenticate, so they are always refused. Reads (f m t s l v) stay open. Three wrong tokens
// hang up. A line that looks like an HTTP request line or an HTTP header (Host:, Origin:, Content-Type: ...) always hangs up: a web page
// cannot talk to this port. The three-argument overload above is a session with required == false (no token), for the tests of the parser.
struct RigctlSession { bool required = false; std::string token; bool authed = false; int badAuth = 0; };
std::string rigctlHandle(const std::string& line, RigctlRadio& radio, bool& quit, RigctlSession& session);

} // namespace dect2::apipol
