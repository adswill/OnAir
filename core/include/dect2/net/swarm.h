// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LAN swarm: several OnAir instances on one private network cooperate. This file is the pure logic (no sockets, no clock, no GUI), so every rule is
// unit-tested with hostile input: announce packets, the node table, merged decoded messages and tracks, range splitting for coordinated scans,
// the one-time pairing handshake with token and rate limit, and memory sync. Off by default. Never carries IQ or audio. Never reaches past the
// local subnet (announces from other subnets are dropped). Remote control needs a paired token, never covers transmit or the voice key store.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace dect2::swarm {

// ---------------------------------------------------------------- 1. discovery packets
struct Announce {
    std::string id;        // 16 lowercase hex characters, random per install
    std::string name;      // <= 32 printable characters
    std::string radio;     // e.g. "RTL-SDR", "HackRF Pro", "none"
    std::string mode;      // current mode, e.g. "ADS-B"
    std::string summary;   // <= 80 characters, e.g. "42 aircraft"
    double loHz = 0, hiHz = 0;   // the range the radio can tune; lo <= hi
    int load = 0;          // 0..100
    int ctlPort = 0;       // the pairing / control port, 0 = not accepting pairing
};
constexpr size_t kMaxAnnounceBytes = 512;
std::string encodeAnnounce(const Announce& a);                                   // "OASWARM1\n" then key=value lines
bool parseAnnounce(const uint8_t* p, size_t n, Announce& out, std::string& err); // strict; out is untouched on failure
std::string announcePreview(const Announce& a);                                  // what the user sees before enabling announcing
bool isPrivateIpv4(uint32_t ip);                                                 // 10/8, 172.16/12, 192.168/16, 169.254/16, 127/8 (host order)
bool onLocalSubnet(uint32_t src, uint32_t local, uint32_t mask);                 // mask must be a contiguous prefix of at least /8

// ---------------------------------------------------------------- 2. swarm view
struct Node {
    Announce info;
    uint32_t ip = 0;
    int64_t lastSeenMs = 0;
};
class NodeTable {
public:
    enum Result { Accepted, BadSource, WrongSubnet, RateLimited, Spoofed, TableFull, SelfId };
    explicit NodeTable(std::string ownId = std::string(), int64_t expireMs = 30000, size_t maxNodes = 64);
    Result update(const Announce& a, uint32_t srcIp, uint32_t localIp, uint32_t mask, int64_t nowMs);
    void expire(int64_t nowMs);
    std::vector<Node> list() const;   // sorted by name, then id
    size_t size() const { return nodes_.size(); }
private:
    std::string own_;
    int64_t expireMs_;
    size_t max_;
    std::map<std::string, Node> nodes_;
    std::map<uint32_t, int64_t> lastFromIp_;   // one packet per second per source
};

// ---------------------------------------------------------------- 3. pooled results
struct Msg {
    std::string kind;     // adsb, ais, acars, aprs, pager, ham, ... (1..16 of a-z 0-9 _ -)
    std::string text;     // the decoded content, <= 512 bytes
    std::string nodeId;
    int64_t timeMs = 0;   // when it was received (sender's clock, checked against ours)
};
struct MergedRow {
    std::string kind, text;
    int64_t timeMs = 0;
    std::vector<std::string> nodes;   // every node that heard it (first one first), at most 16
};
class MessageMerger {
public:
    enum Result { New, Duplicate, Rejected };
    explicit MessageMerger(int64_t windowMs = 2000, size_t maxRows = 5000) : window_(windowMs), max_(maxRows) {}
    // Same kind and same content (case and spacing ignored) within the window is one row with several node tags.
    Result add(const Msg& m, int64_t nowMs);
    const std::deque<MergedRow>& rows() const { return rows_; }
    static std::string normalise(const std::string& text);
private:
    int64_t window_;
    size_t max_;
    std::deque<MergedRow> rows_;
    uint64_t base_ = 0;                                  // sequence number of rows_[0]
    std::unordered_map<std::string, uint64_t> latest_;   // kind + normalised text -> sequence of its newest row
};
struct Track {
    std::string kind, id, nodeId;   // adsb / ais, ICAO or MMSI, node that heard it last
    double lat = 0, lon = 0;
    int64_t timeMs = 0;
    std::vector<std::string> nodes;
};
class TrackBoard {   // the combined ADS-B / AIS map
public:
    explicit TrackBoard(size_t maxTracks = 4000, int64_t keepMs = 120000) : max_(maxTracks), keep_(keepMs) {}
    bool update(const std::string& kind, const std::string& id, double lat, double lon, const std::string& nodeId, int64_t timeMs, int64_t nowMs);
    void expire(int64_t nowMs);
    std::vector<Track> list() const;
private:
    size_t max_;
    int64_t keep_;
    std::map<std::string, Track> t_;
};

// ---------------------------------------------------------------- 4. coordinated scanning
struct Cap {
    std::string id;
    double minHz = 0, maxHz = 0;   // tunable range
    double bwHz = 0;               // instantaneous bandwidth
    int load = 0;                  // 0..100
};
struct Slice { std::string id; double loHz = 0, hiHz = 0; };
struct SplitResult {
    std::vector<Slice> slices;                         // sorted by loHz, adjacent slices of one node merged
    std::vector<std::pair<double, double>> uncovered;  // parts no node can tune
};
bool splitRange(double loHz, double hiHz, const std::vector<Cap>& caps, double stepHz, SplitResult& out, std::string& err);
struct Hit { double freqHz = 0; double level = 0; std::string nodeId; };
struct MergedHit { double freqHz = 0; double level = 0; std::vector<std::string> nodes; };
std::vector<MergedHit> mergeHits(const std::vector<Hit>& hits, double tolHz);   // hits within tolHz are one; the strongest gives frequency and level

// ---------------------------------------------------------------- 5. pairing, tokens, rate limit
using RandFn = std::function<void(uint8_t*, size_t)>;
RandFn systemRandom();
// The pairing secret: 20 random bytes (160 bits) as 32 base32 characters (A-Z, 2-7), shown by the UI in groups of 4 and as a QR code. It is NOT a
// short typed PIN: the repository has no password-authenticated key exchange (no X25519 or SPAKE2 group primitive is exposed; the Ed25519 code is
// not constant-time and has no DH), so a short code would be guessable offline from one recorded exchange. With 160 bits the HMAC proofs below
// cannot be attacked offline. The user carries it to the other node by scanning the QR code, copy and paste, or typing 32 characters.
std::string newPairingCode(const RandFn& rnd);
std::string normalisePairingCode(const std::string& typed);   // removes spaces and dashes, upper-cases; empty when it is not 32 valid base32 characters

// Responder = the node that will be controlled. Lines (all ASCII, <= 200 bytes):
//   C: OAPAIR1 HELLO <32 hex nonce>      S: OAPAIR1 CHAL <32 hex nonce>
//   C: OAPAIR1 PROOF <64 hex>            S: OAPAIR1 OK <64 hex server proof> <48 hex wrapped token>   or   OAPAIR1 ERR <reason>
// proof = HMAC-SHA256(code, "C" nonceC nonceS). The code is never sent. A wrong proof counts; 3 wrong proofs lock the offer. The code works once.
// Threat model: a passive or active attacker on the LAN sees nonces, proofs and the wrapped token but cannot test guesses offline because the code has
// 160 bits. A fake responder cannot produce the server proof without the code, so the initiator detects it. An attacker who sees the screen or QR
// code while it is shown can pair (show it only on the node being controlled; it expires after 2 minutes). The token is then a bearer secret on a
// plain-text channel: later control traffic must use the TLS channel (repo TLS layer, DECT2_WITH_TLS) or be HMAC-signed with a key derived from the
// token; that channel is NOT written yet. A short typed PIN would need a PAKE (SPAKE2/OPAQUE): the repo has no group primitive for it.
class PairResponder {
public:
    enum State { Idle, Offered, Challenged, Locked, Done };
    explicit PairResponder(RandFn rnd = systemRandom()) : rnd_(std::move(rnd)) {}
    bool offer(const std::string& code, int64_t nowMs, int64_t ttlMs = 120000);   // user pressed "Pair a node"; replaces any earlier offer; false unless code came from newPairingCode / normalisePairingCode
    void cancel();
    std::string handle(const std::string& line, const std::string& peerKey, int64_t nowMs);   // reply line, never empty
    State state(int64_t nowMs);
    bool takePaired(std::string& token, std::string& peerKey);   // true once after success
private:
    RandFn rnd_;
    State st_ = Idle;
    std::string code_, peer_, nc_, ns_, token_, donePeer_;
    int64_t expires_ = 0, challengeAt_ = 0;
    int fails_ = 0;
    bool taken_ = true;
};
class PairInitiator {
public:
    explicit PairInitiator(RandFn rnd = systemRandom()) : rnd_(std::move(rnd)) {}
    std::string hello();                                                   // first line
    std::string handle(const std::string& line, const std::string& code); // reply to CHAL (the proof) or empty; on OK sets token()
    bool paired() const { return ok_; }
    bool failed() const { return failed_; }
    const std::string& token() const { return token_; }
private:
    RandFn rnd_;
    std::string nc_, ns_, token_;
    bool ok_ = false, failed_ = false, sentProof_ = false;
};

// Paired tokens. authorise is constant-time per entry. Revocable.
class TokenBook {
public:
    void add(const std::string& token, const std::string& label);
    bool revoke(const std::string& label);
    bool authorise(const std::string& token, std::string* label = nullptr) const;
    // The control channel names the key it uses without sending it: keyIdOf(token) = 16 hex characters of HMAC-SHA256(token, "OACTL1 id").
    bool findByKeyId(const std::string& keyId, std::string& token, std::string* label = nullptr) const;
    std::vector<std::pair<std::string, std::string>> entries() const { return t_; }   // (token, label), to save
    size_t size() const { return t_.size(); }
private:
    std::vector<std::pair<std::string, std::string>> t_;
};
// What a paired peer may ask for: tune, mode, start/stop, gain, rate, scan slice, status. Never transmit, never keys or credentials, never settings.
bool controlAllowed(const std::string& command);
std::string keyIdOf(const std::string& token);   // see TokenBook::findByKeyId

class RateLimiter {   // token bucket, one per peer; allow() false when the peer is too fast
public:
    RateLimiter(double perSecond = 5, double burst = 10) : rate_(perSecond), burst_(burst), tokens_(burst) {}
    bool allow(int64_t nowMs);
private:
    double rate_, burst_, tokens_;
    int64_t last_ = 0;
    bool started_ = false;
};

// ---------------------------------------------------------------- 6. memory sync between the user's own nodes
struct Mem {
    std::string key;     // stable id (<= 48 chars of a-z A-Z 0-9 _ - .)
    std::string name;    // <= 64 printable characters
    std::string mode;    // <= 32
    double freqHz = 0;
    std::string note;    // <= 200
    int64_t modifiedMs = 0;
    bool deleted = false;   // tombstone
};
struct MemMergeStats { int added = 0, updated = 0, removed = 0, kept = 0, rejected = 0; };
bool memValid(const Mem& m);
// Last write wins per key; invalid remote entries are dropped and counted. local is changed in place; at most 10000 entries.
MemMergeStats mergeMemories(std::vector<Mem>& local, const std::vector<Mem>& remote);

} // namespace dect2::swarm
