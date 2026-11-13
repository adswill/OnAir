// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LAN swarm control channel: the encrypted, authenticated TCP link between paired nodes, the commands carried on it (control, pooled results,
// coordinated scan, memory sync), and the pairing transport. Existing primitives only (HMAC-SHA256, AES-256-CTR from meshcrypto); nothing invented
// beyond the standard encrypt-then-MAC framing described below. See swarm.h for the rules and docs/LAN-SWARM.md for the threat model.
//
// Session setup (after pairing both sides hold the token; it is never sent again):
//   C: OACTL1 HELLO <keyId 16 hex> <nonceC 32 hex>\n        S: OACTL1 CHAL <nonceS 32 hex>\n   (or OACTL1 ERR <reason>\n)
// Keys: HMAC-SHA256(token, label | nonceC | nonceS) with labels "enc c2s", "mac c2s", "enc s2c", "mac s2c" (a key per direction).
// Frame (both directions, after the CHAL line): u32 big-endian length N (<= 65536+40) | u64 seq | ciphertext | tag(32)
//   ciphertext = AES-256-CTR(encKey, iv = seq(8 bytes big-endian) | 8 zero bytes) of the plain text; tag = HMAC-SHA256(macKey, seq | ciphertext).
// seq starts at 0 and must be exactly the next number: a replayed, reordered or dropped frame ends the session. A frame that fails the tag is
// never decrypted. The first frame from each side proves it knows the token (a fake peer cannot make a valid tag).
#pragma once
#include "dect2/net/swarm.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2::swarm {

// ---------------------------------------------------------------- framing (pure)
constexpr size_t kMaxPlain = 65536;
class ControlSession {
public:
    ControlSession(const std::string& token, const std::string& nonceC, const std::string& nonceS, bool isClient);
    std::string seal(const std::string& plain);                       // the frame bytes (without the 4 length bytes); empty when plain is too big
    bool open(const std::string& frame, std::string& plain);          // false: bad length, tag or sequence (the session must end)
private:
    uint8_t encOut_[32], macOut_[32], encIn_[32], macIn_[32];
    uint64_t sendSeq_ = 0, recvSeq_ = 0;
};
std::string frameWithLength(const std::string& frame);                // adds the 4 length bytes
bool frameLengthOk(uint32_t n);                                       // the length a reader will accept before allocating

// ---------------------------------------------------------------- commands (pure)
// One request is "name arg arg ...\n<body>" (the body, possibly empty, is everything after the first newline). Names: the control commands of
// controlAllowed() plus status, nodeinfo, results, tracks, scan_slices, mem_get, mem_put. Replies are "OK ...\n<body>" or "ERR <reason>".
struct Command {
    std::string name;
    std::vector<std::string> args;
    std::string body;
};
bool parseCommand(const std::string& plain, Command& out, std::string& err);
bool commandAllowed(const std::string& name);   // controlAllowed() plus the read-only / sync names above; never transmit, keys, settings, tokens
bool parseDouble(const std::string& s, double& v);   // plain decimal, finite

// Pooled results as text: "seq\tkind\ttimeMs\ttext\n" per line (tabs and newlines inside text become spaces).
struct SeqMsg { uint64_t seq = 0; Msg m; };
std::string serializeMsgs(const std::vector<SeqMsg>& v);
std::vector<SeqMsg> parseMsgs(const std::string& s, const std::string& nodeId, size_t maxLines = 500);   // drops bad lines
// Memories as text: "key\tname\tmode\tfreqHz\tnote\tmodifiedMs\tdeleted\n"
std::string serializeMems(const std::vector<Mem>& v);
std::vector<Mem> parseMems(const std::string& s, size_t maxLines = 10000);   // drops lines that do not validate
// Tracks (ADS-B / AIS positions) as text: "kind\tid\tlatE6\tlonE6\ttimeMs\n" (positions in micro-degrees, integers)
struct TrackRow { std::string kind, id; double lat = 0, lon = 0; int64_t timeMs = 0; };
std::string serializeTracks(const std::vector<TrackRow>& v);
std::vector<TrackRow> parseTracks(const std::string& s, size_t maxLines = 2000);
// Scan hits as text: "freqHz\tlevel\n"
std::string serializeHits(const std::vector<std::pair<double, double>>& v);
std::vector<std::pair<double, double>> parseHits(const std::string& s, size_t maxLines = 5000);

// ---------------------------------------------------------------- what a node offers to paired peers
class SwarmHost {
public:
    virtual ~SwarmHost() = default;
    virtual std::string nodeInfo() = 0;                                   // one line, like the announce text
    virtual std::string status() = 0;                                     // one line
    virtual bool start(std::string& err) = 0;
    virtual bool stop(std::string& err) = 0;
    virtual bool setFrequencyHz(double hz, std::string& err) = 0;
    virtual bool setMode(const std::string& modeName, std::string& err) = 0;
    virtual bool setGainDb(double, std::string& err) { err = "not supported on this node"; return false; }
    virtual bool setSampleRateHz(double, std::string& err) { err = "not supported on this node"; return false; }
    // Scan [loHz, hiHz) in steps of stepHz and return what was found (frequency, level). False with err when this node cannot scan.
    virtual bool scanSlice(double, double, double, std::vector<std::pair<double, double>>&, std::string& err) { err = "this node cannot scan yet"; return false; }
    // Decoded messages with sequence number > sinceSeq (at most max), oldest first. Own messages only, never other nodes' (no relaying).
    virtual std::vector<SeqMsg> messagesSince(uint64_t, size_t) { return {}; }
    virtual uint64_t latestSeq() { return 0; }                            // the newest sequence number (a peer that sees it drop knows we restarted)
    virtual std::vector<TrackRow> tracks() { return {}; }                // this node's own aircraft / vessel positions seen in the last minutes
    virtual std::vector<Mem> memories() { return {}; }
    virtual int applyMemories(const std::vector<Mem>&) { return 0; }      // merged by the host; returns how many changed
};

// Runs one request against a host after the whitelist: the function every transport (TCP server, tests) uses.
std::string runCommand(SwarmHost& host, const std::string& plainRequest);

// ---------------------------------------------------------------- TCP server (pairing + control) and client
struct CtlServerStatus {
    bool listening = false;
    int port = 0;
    int sessions = 0;
    uint64_t requests = 0, rejected = 0, paired = 0;
    std::string lastError;
};
class SwarmCtlServer {
public:
    explicit SwarmCtlServer(SwarmHost& host);
    ~SwarmCtlServer();
    // Listens on all interfaces (port 0: any free port). Peers must be on a private address on the same subnet as one of this computer's
    // interfaces (loopback too when allowLoopback, for tests). Opt-in: nothing listens until this is called.
    bool start(int port, bool allowLoopback, std::string& err);
    void stop();
    int port() const;
    CtlServerStatus status() const;
    // Pairing: the UI shows the code (and QR) from startPairing; it expires after 2 minutes, works once, three wrong proofs lock it.
    std::string startPairing();
    void cancelPairing();
    void setOnPaired(std::function<void(const std::string& token, const std::string& peerAddr)> f);   // called from a server thread
    void addToken(const std::string& token, const std::string& label);
    bool revoke(const std::string& label);
    std::vector<std::pair<std::string, std::string>> tokens() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Pair with the node at host:port using the code it shows; on success `token` is the shared secret (store it, with the node's id as the label).
bool swarmPair(const std::string& host, int port, const std::string& typedCode, std::string& token, std::string& err);

class SwarmClient {
public:
    SwarmClient();
    ~SwarmClient();
    bool connect(const std::string& host, int port, const std::string& token, std::string& err);
    bool request(const std::string& plain, std::string& reply, std::string& err);   // one request, one reply
    void setTimeoutMs(int ms);                                                      // how long a reply may take (default 20 s; a scan needs minutes)
    void close();
    bool connected() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// ---------------------------------------------------------------- the three cooperative jobs, over any way of sending a request to a node
using Requester = std::function<bool(const std::string& nodeId, const std::string& plainRequest, std::string& reply)>;

// Pooled results: pull what the node decoded since `since` and add it to the merger (tags: nodeId). Returns how many were new.
int pullResults(const Requester& rq, const std::string& nodeId, uint64_t& since, MessageMerger& into, int64_t nowMs, std::string& err);
// Combined map: pull the node's own tracks into the board (tags: nodeId). Returns how many were accepted.
int pullTracks(const Requester& rq, const std::string& nodeId, TrackBoard& into, int64_t nowMs, std::string& err);
// Coordinated scan: split [lo, hi) across the nodes, send each its slice, merge the hits. Nodes that fail are listed in `failed` and their slice is
// reported in `uncovered` (no node is asked twice).
struct ScanOutcome {
    std::vector<MergedHit> hits;
    std::vector<Slice> slices;
    std::vector<std::pair<double, double>> uncovered;
    std::vector<std::string> failed;
};
bool coordinateScan(const Requester& rq, double loHz, double hiHz, double stepHz, const std::vector<Cap>& caps, double tolHz, ScanOutcome& out, std::string& err);
// Memory sync with one of your own nodes: fetch its memories, merge into `local` (last write wins), send the merged list back. Both sides end equal.
bool syncMemories(const Requester& rq, const std::string& nodeId, std::vector<Mem>& local, MemMergeStats& stats, std::string& err);

}   // namespace dect2::swarm
