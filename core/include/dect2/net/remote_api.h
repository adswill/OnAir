// Remote-control API: a small HTTP/1.1 + WebSocket (RFC 6455) server so scripts and other programs can list the radios, start and stop the receiver,
// set the mode, frequency, sample rate and gain, read the telemetry as JSON, subscribe to telemetry and the spectrum over a WebSocket, and fetch
// the latest spectrum line. No dependencies (sockets only). Off until the user turns it on; it listens on 127.0.0.1 unless "lan" is set, and a
// connection from any other address needs the token (lan without a token is refused). The endpoints are listed in README.md.
//
// Hardening: header block <= 8 KB, body <= 4 KB, 5 s to send a request, 16 connections (8 WebSockets), POST needs Content-Type: application/json
// (so a web page cannot send one without a CORS preflight, which is never granted: the server sends no CORS headers), a request that carries an
// Origin header must come from the server's own origin, and a loopback request must name a loopback Host (DNS rebinding).
#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

struct JsonVal;

struct RemoteApiConfig {
    bool on = false;
    int port = 8765;          // 0: any free port (tests); config() then tells which
    bool lan = false;         // listen on every address (else 127.0.0.1 only); needs a token
    std::string token;        // >= 16 characters; required from every non-loopback peer (Authorization: Bearer <token>, or ?token=)
    bool requireTokenLocal = false;   // a program on this computer needs the token too (the app turns it on and makes the token on first use)
    double ratePerSec = 0, rateBurst = 40;   // requests per second and burst allowed from one address (HTTP 429 beyond); 0 rate: no limit
    std::string tlsCert, tlsKey;   // PEM files: serve HTTPS / wss instead of HTTP / ws (needs a build with OpenSSL; both or neither)
};

struct RemoteApiStatus {
    bool listening = false;
    std::string lastError;
    bool tls = false;                     // listening with TLS
    int httpClients = 0, wsClients = 0;   // connections open now
    uint64_t requests = 0, rejected = 0;  // handled / refused (bad auth, bad request, too many connections)
};

// What the program behind the API offers. The server calls it from its connection threads, one call at a time (the server serialises them), so a
// host that owns a GUI must hand work to its own thread. Every *Json function returns a complete JSON value; the setters return false with err set.
class RemoteApiHost {
public:
    virtual ~RemoteApiHost() = default;
    virtual std::string statusJson() = 0;                         // running, device, mode, frequency, rate, gain
    virtual std::string devicesJson() = 0;                        // the radios and sources the host knows
    virtual std::string modesJson() = 0;                          // the modes (family numbers) setMode accepts
    virtual std::string telemetryJson(uint64_t& seq) = 0;         // the active mode's telemetry; seq changes when it is a new report
    virtual std::string spectrumJson(int maxBins, uint64_t& seq) = 0;   // the latest spectrum line (at most maxBins values)
    virtual bool start(int deviceIndex, std::string& err) = 0;    // deviceIndex < 0: the selected one
    virtual bool stop(std::string& err) = 0;
    virtual bool setMode(int family, const std::string& subId, std::string& err) = 0;
    virtual bool setFrequencyHz(double hz, std::string& err) = 0;
    virtual bool setSampleRateHz(double hz, std::string& err) = 0;   // 0: automatic
    virtual bool setGainDb(double db, std::string& err) = 0;
    // ---- optional (headless server): a built-in web page, the spectrum as bytes, audio for WebSocket clients
    virtual std::string indexHtml() { return std::string(); }     // served at GET / and /index.html (empty: 404)
    // The latest spectrum line as one binary WebSocket message: [0x01][u32 seq][f32 centreHz][f32 rateHz][f32 floorDb][f32 topDb][bins bytes, 0 = floorDb, 255 = topDb], little endian.
    virtual bool spectrumBinary(int, uint64_t&, std::string&) { return false; }
    virtual int audioOpen(int /*rateHz*/) { return -1; }           // a listener at this rate (mono, 16 bit); -1: the host has no audio
    virtual bool audioRead(int, std::string& /*pcm16le*/) { return false; }   // the audio since the last call (empty: none yet); false: the listener is gone
    virtual void audioClose(int) {}
    // ---- optional: the rest of the control surface (docs/REMOTE-API.md). A getter returning an empty string / a setter returning false with err
    // "not supported" make the server answer 501. None of these can transmit or touch keys, credentials or security settings.
    virtual std::string sessionsJson(const std::string& query) { (void)query; return std::string(); }   // calls, threads and tracks, paged (results/sessions_json.h); empty = not offered
    virtual std::string memoriesJson() { return std::string(); }      // the memory list (read only)
    virtual std::string schedulerJson() { return std::string(); }     // the scheduled jobs (read only)
    virtual std::string audioJson() { return std::string(); }         // volume, mute
    virtual std::string recordingJson() { return std::string(); }     // is a recording running, which file, how long
    virtual bool setVolume(double /*level0to1*/, std::string& err) { err = "not supported"; return false; }
    virtual bool setMute(bool /*on*/, std::string& err) { err = "not supported"; return false; }
    virtual bool setSquelch(bool /*on*/, bool /*autoLevel*/, double /*levelDb*/, std::string& err) { err = "not supported"; return false; }
    virtual bool setBandwidthHz(double /*hz*/, std::string& err) { err = "not supported"; return false; }
    virtual bool setAutoGain(bool /*on*/, std::string& err) { err = "not supported"; return false; }
    virtual bool recordStart(const std::string& /*kind: "iq" or "audio"*/, std::string& err) { err = "not supported"; return false; }
    virtual bool recordStop(std::string& err) { err = "not supported"; return false; }
    // Transmit (3.2): the operator's explicit command is the decision; the answer carries "warnings" (band, licence, gain, ...). OnAir blocks no mode, band or power.
    virtual std::string txStatusJson() { return std::string(); }                                                                  // empty: this program has no transmitter
    virtual bool txStart(const std::map<std::string, JsonVal>& /*body*/, std::string& /*json*/, std::string& err) { err = "not supported"; return false; }
    virtual bool txStop(std::string& /*json*/, std::string& err) { err = "not supported"; return false; }
    // Decoded messages (one JSON line each, see apipol::eventLine). cursor UINT64_MAX: from now. Fills `out` with the lines since the cursor and moves it. false: no message stream.
    virtual bool eventsSince(uint64_t& /*cursor*/, size_t /*maxN*/, std::vector<std::string>& /*out*/, bool& /*gap*/) { return false; }
    // GET / POST under /api/v1/x/ (the program's own extras). Returns the HTTP status (0: not handled, the server answers 404) and the JSON body in `json`.
    virtual int handleExtra(const std::string& /*method*/, const std::string& /*path*/, const std::map<std::string, JsonVal>& /*body*/, std::string& /*json*/) { return 0; }
};

class RemoteApiServer {
public:
    explicit RemoteApiServer(RemoteApiHost& host);
    ~RemoteApiServer();
    // Starts, restarts or stops the server as the config says. False (and status().lastError) when it cannot listen or lan has no usable token.
    bool configure(const RemoteApiConfig& c);
    RemoteApiConfig config() const;
    RemoteApiStatus status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// A fresh random token of 24 hex characters
std::string remoteApiNewToken();

// ---- helpers the server is built from (public so they can be tested)
std::string sha1(const std::string& data);                 // 20 raw bytes
std::string base64(const std::string& data);
std::string wsAcceptKey(const std::string& clientKey);     // base64(sha1(key + GUID)), RFC 6455 4.2.2
std::string jsonEscape(const std::string& s);              // the content of a JSON string (no quotes)
std::string jsonNum(double v);                             // a number, or null for NaN / inf
// one WebSocket frame, server to client (never masked)
std::string wsFrame(int opcode, const std::string& payload);

struct JsonVal { enum Kind { Null, Num, Str, Bool } kind = Null; double num = 0; std::string str; bool b = false; };
// A flat JSON object ({"a": 1, "b": "x", "c": true}); nesting, arrays and anything malformed give false. Keys are kept in `out`.
bool parseFlatJson(const std::string& text, std::map<std::string, JsonVal>& out);

} // namespace dect2
