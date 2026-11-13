// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LAN swarm control channel. See dect2/net/swarm_ctl.h for the wire format and the rules.
#include "dect2/net/swarm_ctl.h"
#include "dect2/mesh/mesh_crypto.h"
#include "dect2/net/swarm_net.h"
#include "tcp_client.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <thread>

namespace dect2::swarm {
namespace {

int64_t steadyMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void derive(const std::string& token, const std::string& label, const std::string& nc, const std::string& ns, uint8_t out[32]) {
    std::string m = label + "|" + nc + "|" + ns;
    meshcrypto::hmacSha256((const uint8_t*)token.data(), token.size(), (const uint8_t*)m.data(), m.size(), out);
}
bool ctEq(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}
void putBe64(uint8_t* p, uint64_t v) { for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; } }
uint64_t getBe64(const uint8_t* p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | p[i]; return v; }
std::string hex(const uint8_t* p, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s.push_back(h[p[i] >> 4]); s.push_back(h[p[i] & 15]); }
    return s;
}
bool isHex(const std::string& s, size_t n) {
    if (s.size() != n) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
std::string oneLine(const std::string& s, size_t maxLen = 200) {
    std::string o;
    for (unsigned char c : s) { if (o.size() >= maxLen) break; o.push_back((c < 0x20 || c == 0x7f) ? ' ' : (char)c); }
    return o;
}
std::string tabClean(const std::string& s) {
    std::string o = s;
    for (char& c : o) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return o;
}
std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> v;
    size_t p = 0;
    for (;;) {
        size_t e = line.find('\t', p);
        if (e == std::string::npos) { v.push_back(line.substr(p)); break; }
        v.push_back(line.substr(p, e - p));
        p = e + 1;
    }
    return v;
}
bool parseU64(const std::string& s, uint64_t& v) {
    if (s.empty() || s.size() > 19) return false;
    v = 0;
    for (char c : s) { if (c < '0' || c > '9') return false; v = v * 10 + (uint64_t)(c - '0'); }
    return true;
}
bool parseI64(const std::string& s, int64_t& v) {
    uint64_t u;
    if (!parseU64(s, u) || u > (uint64_t)INT64_MAX) return false;
    v = (int64_t)u;
    return true;
}
constexpr size_t kReplyBudget = 60000;
}   // namespace

// ---------------------------------------------------------------- framing
ControlSession::ControlSession(const std::string& token, const std::string& nc, const std::string& ns, bool isClient) {
    uint8_t ec[32], mc[32], es[32], ms[32];
    derive(token, "enc c2s", nc, ns, ec); derive(token, "mac c2s", nc, ns, mc);
    derive(token, "enc s2c", nc, ns, es); derive(token, "mac s2c", nc, ns, ms);
    std::memcpy(encOut_, isClient ? ec : es, 32); std::memcpy(macOut_, isClient ? mc : ms, 32);
    std::memcpy(encIn_, isClient ? es : ec, 32);  std::memcpy(macIn_, isClient ? ms : mc, 32);
}
std::string ControlSession::seal(const std::string& plain) {
    if (plain.size() > kMaxPlain) return std::string();
    uint8_t iv[16] = {};
    putBe64(iv, sendSeq_);
    std::string f(8 + plain.size() + 32, '\0');
    putBe64((uint8_t*)&f[0], sendSeq_);
    std::memcpy(&f[8], plain.data(), plain.size());
    if (!plain.empty()) meshcrypto::aesCtr(encOut_, 32, iv, (uint8_t*)&f[8], plain.size());
    uint8_t tag[32];
    meshcrypto::hmacSha256(macOut_, 32, (const uint8_t*)f.data(), 8 + plain.size(), tag);
    std::memcpy(&f[8 + plain.size()], tag, 32);
    sendSeq_++;
    return f;
}
bool ControlSession::open(const std::string& f, std::string& plain) {
    if (f.size() < 40 || f.size() > 40 + kMaxPlain) return false;
    size_t n = f.size() - 40;
    uint8_t tag[32];
    meshcrypto::hmacSha256(macIn_, 32, (const uint8_t*)f.data(), 8 + n, tag);
    if (!ctEq(tag, (const uint8_t*)&f[8 + n], 32)) return false;   // checked before anything is decrypted
    uint64_t seq = getBe64((const uint8_t*)f.data());
    if (seq != recvSeq_) return false;
    uint8_t iv[16] = {};
    putBe64(iv, seq);
    std::string p = f.substr(8, n);
    if (n) meshcrypto::aesCtr(encIn_, 32, iv, (uint8_t*)&p[0], n);
    recvSeq_++;
    plain = std::move(p);
    return true;
}
std::string frameWithLength(const std::string& frame) {
    uint32_t n = (uint32_t)frame.size();
    std::string s(4, '\0');
    s[0] = (char)(n >> 24); s[1] = (char)(n >> 16); s[2] = (char)(n >> 8); s[3] = (char)n;
    return s + frame;
}
bool frameLengthOk(uint32_t n) { return n >= 40 && n <= 40 + kMaxPlain; }

// ---------------------------------------------------------------- commands
bool parseDouble(const std::string& s, double& v) {
    if (s.empty() || s.size() > 24) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == 'e' || c == 'E' || c == '+')) return false;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end && *end == 0 && std::isfinite(v);
}
bool commandAllowed(const std::string& n) {
    static const char* more[] = {"nodeinfo", "results", "tracks", "scan_slices", "mem_get", "mem_put"};
    if (controlAllowed(n)) return true;
    for (const char* m : more) if (n == m) return true;
    return false;
}
bool parseCommand(const std::string& plain, Command& out, std::string& err) {
    if (plain.empty() || plain.size() > kMaxPlain) { err = "size"; return false; }
    size_t nl = plain.find('\n');
    std::string head = plain.substr(0, nl);
    std::string body = nl == std::string::npos ? std::string() : plain.substr(nl + 1);
    if (head.size() > 512) { err = "line too long"; return false; }
    for (unsigned char c : head) if (c < 0x20 || c == 0x7f) { err = "control character"; return false; }
    Command c;
    size_t p = 0;
    std::vector<std::string> w;
    while (p <= head.size()) {
        size_t e = head.find(' ', p);
        std::string t = head.substr(p, e == std::string::npos ? std::string::npos : e - p);
        if (!t.empty()) w.push_back(t);
        if (e == std::string::npos) break;
        p = e + 1;
    }
    if (w.empty() || w.size() > 12) { err = "words"; return false; }
    if (w[0].size() > 24) { err = "name"; return false; }
    for (char ch : w[0]) if (!((ch >= 'a' && ch <= 'z') || ch == '_')) { err = "name"; return false; }
    for (size_t i = 1; i < w.size(); i++) if (w[i].size() > 64) { err = "argument too long"; return false; }
    c.name = w[0];
    c.args.assign(w.begin() + 1, w.end());
    c.body = std::move(body);
    out = std::move(c);
    return true;
}

std::string serializeMsgs(const std::vector<SeqMsg>& v) {
    std::string o;
    for (auto& s : v) {
        std::string l = std::to_string(s.seq) + "\t" + s.m.kind + "\t" + std::to_string(s.m.timeMs) + "\t" + tabClean(s.m.text) + "\n";
        if (o.size() + l.size() > kReplyBudget) break;
        o += l;
    }
    return o;
}
std::vector<SeqMsg> parseMsgs(const std::string& s, const std::string& nodeId, size_t maxLines) {
    std::vector<SeqMsg> out;
    size_t p = 0, lines = 0;
    while (p < s.size() && lines++ < maxLines) {
        size_t e = s.find('\n', p);
        std::string l = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? s.size() : e + 1;
        auto f = splitTabs(l);
        SeqMsg m;
        if (f.size() != 4 || !parseU64(f[0], m.seq) || !parseI64(f[2], m.m.timeMs)) continue;
        m.m.kind = f[1]; m.m.text = f[3]; m.m.nodeId = nodeId;
        if (m.m.text.empty() || m.m.text.size() > 512 || f[1].empty() || f[1].size() > 16) continue;
        bool bad = false;
        for (unsigned char c : m.m.text) if (c < 0x20 || c == 0x7f) bad = true;
        if (!bad) out.push_back(std::move(m));
    }
    return out;
}
std::string serializeMems(const std::vector<Mem>& v) {
    std::string o;
    char b[48];
    for (auto& m : v) {
        snprintf(b, sizeof b, "%.0f", m.freqHz);
        std::string l = tabClean(m.key) + "\t" + tabClean(m.name) + "\t" + tabClean(m.mode) + "\t" + b + "\t" + tabClean(m.note) + "\t" + std::to_string(m.modifiedMs) + "\t" + (m.deleted ? "1" : "0") + "\n";
        if (o.size() + l.size() > kReplyBudget) break;
        o += l;
    }
    return o;
}
std::vector<Mem> parseMems(const std::string& s, size_t maxLines) {
    std::vector<Mem> out;
    size_t p = 0, lines = 0;
    while (p < s.size() && lines++ < maxLines) {
        size_t e = s.find('\n', p);
        std::string l = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? s.size() : e + 1;
        auto f = splitTabs(l);
        Mem m;
        double hz;
        if (f.size() != 7 || !parseDouble(f[3], hz) || !parseI64(f[5], m.modifiedMs) || (f[6] != "0" && f[6] != "1")) continue;
        m.key = f[0]; m.name = f[1]; m.mode = f[2]; m.freqHz = hz; m.note = f[4]; m.deleted = f[6] == "1";
        if (memValid(m)) out.push_back(std::move(m));
    }
    return out;
}
std::string serializeTracks(const std::vector<TrackRow>& v) {
    std::string o;
    for (auto& t : v) {
        std::string l = tabClean(t.kind) + "\t" + tabClean(t.id) + "\t" + std::to_string((long long)std::llround(t.lat * 1e6)) + "\t" + std::to_string((long long)std::llround(t.lon * 1e6)) + "\t" + std::to_string(t.timeMs) + "\n";
        if (o.size() + l.size() > kReplyBudget) break;
        o += l;
    }
    return o;
}
std::vector<TrackRow> parseTracks(const std::string& s, size_t maxLines) {
    std::vector<TrackRow> out;
    size_t p = 0, lines = 0;
    while (p < s.size() && lines++ < maxLines) {
        size_t e = s.find('\n', p);
        std::string l = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? s.size() : e + 1;
        auto f = splitTabs(l);
        TrackRow t;
        double la, lo;
        if (f.size() != 5 || !parseDouble(f[2], la) || !parseDouble(f[3], lo) || !parseI64(f[4], t.timeMs)) continue;
        t.kind = f[0]; t.id = f[1]; t.lat = la / 1e6; t.lon = lo / 1e6;
        if (t.kind.empty() || t.kind.size() > 16 || t.id.empty() || t.id.size() > 16) continue;
        out.push_back(std::move(t));   // the board validates the rest (ranges, characters)
    }
    return out;
}
std::string serializeHits(const std::vector<std::pair<double, double>>& v) {
    std::string o;
    char b[64];
    for (auto& h : v) {
        snprintf(b, sizeof b, "%.0f\t%.2f\n", h.first, h.second);
        if (o.size() + strlen(b) > kReplyBudget) break;
        o += b;
    }
    return o;
}
std::vector<std::pair<double, double>> parseHits(const std::string& s, size_t maxLines) {
    std::vector<std::pair<double, double>> out;
    size_t p = 0, lines = 0;
    while (p < s.size() && lines++ < maxLines) {
        size_t e = s.find('\n', p);
        std::string l = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? s.size() : e + 1;
        auto f = splitTabs(l);
        double hz, lv;
        if (f.size() == 2 && parseDouble(f[0], hz) && parseDouble(f[1], lv) && hz > 0 && hz <= 1e12) out.push_back({hz, lv});
    }
    return out;
}

std::string runCommand(SwarmHost& h, const std::string& req) {
    Command c;
    std::string e;
    if (!parseCommand(req, c, e)) return "ERR bad request";
    if (!commandAllowed(c.name)) return "ERR not allowed";
    auto fail = [&](const std::string& m) { return "ERR " + oneLine(m); };
    std::string err;
    double a = 0, b = 0, st = 0;
    if (c.name == "status") return "OK " + oneLine(h.status());
    if (c.name == "nodeinfo") return "OK " + oneLine(h.nodeInfo());
    if (c.name == "start") return h.start(err) ? "OK" : fail(err);
    if (c.name == "stop") return h.stop(err) ? "OK" : fail(err);
    if (c.name == "set_frequency") {
        if (c.args.size() != 1 || !parseDouble(c.args[0], a) || a < 0 || a > 1e12) return fail("need one frequency in Hz");
        return h.setFrequencyHz(a, err) ? "OK" : fail(err);
    }
    if (c.name == "set_mode") {
        std::string m;
        for (auto& w : c.args) m += (m.empty() ? "" : " ") + w;
        if (m.empty()) return fail("need a mode name");
        return h.setMode(m, err) ? "OK" : fail(err);
    }
    if (c.name == "set_gain") {
        if (c.args.size() != 1 || !parseDouble(c.args[0], a) || a < 0 || a > 200) return fail("need one gain in dB");
        return h.setGainDb(a, err) ? "OK" : fail(err);
    }
    if (c.name == "set_rate") {
        if (c.args.size() != 1 || !parseDouble(c.args[0], a) || a < 0 || a > 1e10) return fail("need one rate in Hz");
        return h.setSampleRateHz(a, err) ? "OK" : fail(err);
    }
    if (c.name == "scan_slice") {
        if (c.args.size() != 3 || !parseDouble(c.args[0], a) || !parseDouble(c.args[1], b) || !parseDouble(c.args[2], st) || !(a >= 0) || !(b > a) || b > 1e12 || !(st > 0) || (b - a) / st > 100000)
            return fail("need lo hi step in Hz");
        std::vector<std::pair<double, double>> hits;
        if (!h.scanSlice(a, b, st, hits, err)) return fail(err);
        return "OK\n" + serializeHits(hits);
    }
    if (c.name == "scan_slices") {
        if (c.args.size() != 1 || !parseDouble(c.args[0], st) || !(st > 0)) return fail("need the step in Hz");
        std::vector<std::pair<double, double>> all;
        size_t p = 0;
        int slices = 0;
        while (p < c.body.size()) {
            size_t nl = c.body.find('\n', p);
            auto f = splitTabs(c.body.substr(p, nl == std::string::npos ? std::string::npos : nl - p));
            p = nl == std::string::npos ? c.body.size() : nl + 1;
            if (f.empty() || (f.size() == 1 && f[0].empty())) continue;
            if (++slices > 200) return fail("too many slices");
            if (f.size() != 2 || !parseDouble(f[0], a) || !parseDouble(f[1], b) || !(a >= 0) || !(b > a) || b > 1e12 || (b - a) / st > 100000) return fail("bad slice");
            std::vector<std::pair<double, double>> hits;
            if (!h.scanSlice(a, b, st, hits, err)) return fail(err);
            all.insert(all.end(), hits.begin(), hits.end());
        }
        if (!slices) return fail("no slices");
        return "OK\n" + serializeHits(all);
    }
    if (c.name == "results") {
        uint64_t since = 0;
        if (c.args.size() > 1 || (c.args.size() == 1 && !parseU64(c.args[0], since))) return fail("need an optional sequence number");
        return "OK " + std::to_string(h.latestSeq()) + "\n" + serializeMsgs(h.messagesSince(since, 200));
    }
    if (c.name == "tracks") return "OK\n" + serializeTracks(h.tracks());
    if (c.name == "mem_get") return "OK\n" + serializeMems(h.memories());
    if (c.name == "mem_put") {
        std::vector<Mem> m = parseMems(c.body);
        return "OK " + std::to_string(h.applyMemories(m));
    }
    return "ERR not allowed";
}

// ---------------------------------------------------------------- pooled jobs
int pullResults(const Requester& rq, const std::string& nodeId, uint64_t& since, MessageMerger& into, int64_t nowMs, std::string& err) {
    std::string reply;
    if (!rq(nodeId, "results " + std::to_string(since), reply)) { err = "no answer"; return 0; }
    if (reply.compare(0, 3, "OK ") != 0) { err = reply.substr(0, 80); return 0; }
    size_t nl = reply.find('\n');
    uint64_t latest = 0;
    if (!parseU64(reply.substr(3, nl == std::string::npos ? std::string::npos : nl - 3), latest)) { err = "bad reply"; return 0; }
    if (latest < since) { since = 0; return 0; }   // the node restarted: start again from its beginning next time
    int added = 0;
    uint64_t top = since;
    for (auto& m : parseMsgs(nl == std::string::npos ? std::string() : reply.substr(nl + 1), nodeId)) {
        top = std::max(top, m.seq);
        if (into.add(m.m, nowMs) == MessageMerger::New) added++;
    }
    since = top;
    return added;
}

int pullTracks(const Requester& rq, const std::string& nodeId, TrackBoard& into, int64_t nowMs, std::string& err) {
    std::string reply;
    if (!rq(nodeId, "tracks", reply)) { err = "no answer"; return 0; }
    if (reply.compare(0, 2, "OK") != 0) { err = reply.substr(0, 80); return 0; }
    size_t nl = reply.find('\n');
    int n = 0;
    for (auto& t : parseTracks(nl == std::string::npos ? std::string() : reply.substr(nl + 1)))
        if (into.update(t.kind, t.id, t.lat, t.lon, nodeId, t.timeMs, nowMs)) n++;
    return n;
}

bool coordinateScan(const Requester& rq, double lo, double hi, double step, const std::vector<Cap>& caps, double tol, ScanOutcome& out, std::string& err) {
    out = ScanOutcome();
    SplitResult sr;
    if (!splitRange(lo, hi, caps, step, sr, err)) return false;
    out.slices = sr.slices;
    out.uncovered = sr.uncovered;
    std::map<std::string, std::vector<Slice>> perNode;
    for (auto& s : sr.slices) perNode[s.id].push_back(s);
    std::vector<Hit> hits;
    char b[64];
    for (auto& kv : perNode) {
        std::string body = "scan_slices ";
        snprintf(b, sizeof b, "%.0f\n", step);
        body += b;
        int n = 0;
        for (auto& s : kv.second) {
            if (++n > 200) break;
            snprintf(b, sizeof b, "%.0f\t%.0f\n", s.loHz, s.hiHz);
            body += b;
        }
        std::string reply;
        if (!rq(kv.first, body, reply) || reply.compare(0, 2, "OK") != 0) {
            out.failed.push_back(kv.first);
            for (auto& s : kv.second) out.uncovered.push_back({s.loHz, s.hiHz});
            continue;
        }
        size_t nl = reply.find('\n');
        for (auto& h : parseHits(nl == std::string::npos ? std::string() : reply.substr(nl + 1))) {
            bool inside = false;
            for (auto& s : kv.second) if (h.first >= s.loHz - tol && h.first < s.hiHz + tol) inside = true;   // a node cannot report outside the slice it was given
            if (inside) hits.push_back({h.first, h.second, kv.first});
        }
    }
    out.hits = mergeHits(hits, tol);
    std::sort(out.uncovered.begin(), out.uncovered.end());
    return true;
}

bool syncMemories(const Requester& rq, const std::string& nodeId, std::vector<Mem>& local, MemMergeStats& stats, std::string& err) {
    std::string reply;
    if (!rq(nodeId, "mem_get", reply)) { err = "no answer"; return false; }
    if (reply.compare(0, 2, "OK") != 0) { err = reply.substr(0, 80); return false; }
    size_t nl = reply.find('\n');
    stats = mergeMemories(local, parseMems(nl == std::string::npos ? std::string() : reply.substr(nl + 1)));
    std::string body = serializeMems(local);
    if (std::count(body.begin(), body.end(), '\n') != (long)local.size()) { err = "too many memories to send in one go"; return false; }
    if (!rq(nodeId, "mem_put\n" + body, reply) || reply.compare(0, 2, "OK") != 0) { err = "the node did not accept the memories"; return false; }
    return true;
}

// ---------------------------------------------------------------- sockets
namespace {
bool readLine(sock_t s, std::string& line, size_t maxLen) {
    line.clear();
    for (;;) {
        char c;
        long long k = sockRecv(s, &c, 1);
        if (k <= 0) return false;
        if (c == '\n') return true;
        if (line.size() >= maxLen) return false;
        line.push_back(c);
    }
}
bool readFrame(sock_t s, std::string& frame) {
    uint8_t h[4];
    if (!sockRecvAll(s, h, 4)) return false;
    uint32_t n = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
    if (!frameLengthOk(n)) return false;
    frame.assign(n, '\0');
    return sockRecvAll(s, &frame[0], n);
}
std::string addrText(const sockaddr_in& a) {
    char b[48];
    inet_ntop(AF_INET, &a.sin_addr, b, sizeof b);
    return std::string(b) + ":" + std::to_string(ntohs(a.sin_port));
}
}   // namespace

struct SwarmCtlServer::Impl {
    SwarmHost& host;
    explicit Impl(SwarmHost& h) : host(h) {}
    mutable std::mutex mu;       // book, pairing, stats, callbacks
    std::mutex hostMu;           // one command at a time into the host
    TokenBook book;
    PairResponder pair;
    CtlServerStatus st;
    std::function<void(const std::string&, const std::string&)> onPaired;
    sock_t ls = kBadSock;
    std::thread acceptor;
    std::atomic<bool> stopFlag{false};
    std::atomic<int> active{0};
    std::set<sock_t> conns;
    bool allowLoop = false;

    bool peerAllowed(uint32_t ip) {
        if ((ip >> 24) == 127) return allowLoop;
        if (!isPrivateIpv4(ip)) return false;
        for (auto& f : privateInterfaces()) if (onLocalSubnet(ip, f.ip, f.mask)) return true;
        return false;
    }
    void handle(sock_t s, std::string peer) {
        sockRecvTimeout(s, 5000);
        std::string line;
        if (!readLine(s, line, 300)) return;
        if (line.compare(0, 8, "OAPAIR1 ") == 0) {
            for (int i = 0; i < 4; i++) {
                std::string reply;
                {
                    std::lock_guard<std::mutex> g(mu);
                    reply = pair.handle(line, peer, steadyMs());
                }
                if (!sockSendAll(s, (reply + "\n").data(), reply.size() + 1)) return;
                if (reply.compare(0, 11, "OAPAIR1 OK ") == 0) {
                    std::string token, who;
                    std::function<void(const std::string&, const std::string&)> cb;
                    {
                        std::lock_guard<std::mutex> g(mu);
                        if (pair.takePaired(token, who)) { book.add(token, "paired " + peer.substr(0, peer.find(':'))); st.paired++; cb = onPaired; }
                    }
                    if (cb) cb(token, peer);
                    return;
                }
                if (reply.compare(0, 12, "OAPAIR1 CHAL") != 0) { std::lock_guard<std::mutex> g(mu); st.rejected++; return; }
                if (!readLine(s, line, 300)) return;
            }
            return;
        }
        // OACTL1 HELLO <keyId> <nonceC>
        std::vector<std::string> w;
        size_t p = 0;
        while (p <= line.size()) { size_t e = line.find(' ', p); w.push_back(line.substr(p, e == std::string::npos ? std::string::npos : e - p)); if (e == std::string::npos) break; p = e + 1; }
        if (w.size() != 4 || w[0] != "OACTL1" || w[1] != "HELLO" || !isHex(w[2], 16) || !isHex(w[3], 32)) { std::lock_guard<std::mutex> g(mu); st.rejected++; return; }
        std::string token, label;
        { std::lock_guard<std::mutex> g(mu); if (!book.findByKeyId(w[2], token, &label)) { st.rejected++; const char* r = "OACTL1 ERR unknown\n"; sockSendAll(s, r, strlen(r)); return; } }
        uint8_t nsb[16];
        systemRandom()(nsb, 16);
        std::string ns = hex(nsb, 16);
        std::string chal = "OACTL1 CHAL " + ns + "\n";
        if (!sockSendAll(s, chal.data(), chal.size())) return;
        ControlSession sess(token, w[3], ns, false);
        RateLimiter rl(20, 40);
        sockRecvTimeout(s, 60000);
        { std::lock_guard<std::mutex> g(mu); st.sessions++; }
        for (;;) {
            std::string frame, plain;
            if (stopFlag || !readFrame(s, frame) || !sess.open(frame, plain)) break;
            std::string tok2;
            { std::lock_guard<std::mutex> g(mu); if (!book.findByKeyId(w[2], tok2)) break; st.requests++; }   // revoked while connected: end it
            std::string reply;
            if (!rl.allow(steadyMs())) reply = "ERR rate limited";
            else { std::lock_guard<std::mutex> g(hostMu); reply = runCommand(host, plain); }
            if (reply.size() > kMaxPlain) reply = "ERR reply too large";
            std::string out = frameWithLength(sess.seal(reply));
            if (!sockSendAll(s, out.data(), out.size())) break;
        }
        std::lock_guard<std::mutex> g(mu);
        st.sessions--;
    }
    void acceptLoop() {
        while (!stopFlag) {
            fd_set rd;
            FD_ZERO(&rd); FD_SET(ls, &rd);
            timeval tv{0, 300000};
            if (select((int)ls + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
            sockaddr_in sa{};
            socklen_t sl = sizeof sa;
            sock_t c = accept(ls, (sockaddr*)&sa, &sl);
            if (c == kBadSock) continue;
#if defined(SO_NOSIGPIPE) && !defined(_WIN32)
            { int one = 1; setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
            if (!peerAllowed(ntohl(sa.sin_addr.s_addr)) || active >= 8) {
                { std::lock_guard<std::mutex> g(mu); st.rejected++; }
                sockClose(c);
                continue;
            }
            { std::lock_guard<std::mutex> g(mu); conns.insert(c); }
            active++;
            std::string peer = addrText(sa);
            std::thread([this, c, peer] {
                handle(c, peer);
                { std::lock_guard<std::mutex> g(mu); conns.erase(c); }
                sockClose(c);
                active--;
            }).detach();
        }
    }
};

SwarmCtlServer::SwarmCtlServer(SwarmHost& host) : p_(new Impl(host)) {}
SwarmCtlServer::~SwarmCtlServer() { stop(); }

bool SwarmCtlServer::start(int port, bool allowLoopback, std::string& err) {
    stop();
    Impl& d = *p_;
    netInit();
    d.ls = socket(AF_INET, SOCK_STREAM, 0);
    if (d.ls == kBadSock) { err = "cannot open a socket"; return false; }
    int one = 1;
    setsockopt(d.ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(d.ls, (sockaddr*)&a, sizeof a) != 0 || listen(d.ls, 8) != 0) { err = "cannot listen on port " + std::to_string(port); sockClose(d.ls); d.ls = kBadSock; return false; }
    socklen_t sl = sizeof a;
    getsockname(d.ls, (sockaddr*)&a, &sl);
    d.allowLoop = allowLoopback;
    d.stopFlag = false;
    { std::lock_guard<std::mutex> g(d.mu); d.st = CtlServerStatus(); d.st.listening = true; d.st.port = ntohs(a.sin_port); }
    d.acceptor = std::thread([&d] { d.acceptLoop(); });
    return true;
}
void SwarmCtlServer::stop() {
    Impl& d = *p_;
    d.stopFlag = true;
    if (d.acceptor.joinable()) d.acceptor.join();
    if (d.ls != kBadSock) { sockClose(d.ls); d.ls = kBadSock; }
    { std::lock_guard<std::mutex> g(d.mu); for (sock_t c : d.conns) sockShutdown(c); }
    for (int i = 0; i < 100 && d.active > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(30));
    std::lock_guard<std::mutex> g(d.mu);
    d.st.listening = false;
}
int SwarmCtlServer::port() const { std::lock_guard<std::mutex> g(p_->mu); return p_->st.port; }
CtlServerStatus SwarmCtlServer::status() const { std::lock_guard<std::mutex> g(p_->mu); return p_->st; }
std::string SwarmCtlServer::startPairing() {
    std::string code = newPairingCode(systemRandom());
    std::lock_guard<std::mutex> g(p_->mu);
    p_->pair.offer(code, steadyMs());
    return code;
}
void SwarmCtlServer::cancelPairing() { std::lock_guard<std::mutex> g(p_->mu); p_->pair.cancel(); }
void SwarmCtlServer::setOnPaired(std::function<void(const std::string&, const std::string&)> f) { std::lock_guard<std::mutex> g(p_->mu); p_->onPaired = std::move(f); }
void SwarmCtlServer::addToken(const std::string& t, const std::string& l) { std::lock_guard<std::mutex> g(p_->mu); p_->book.add(t, l); }
bool SwarmCtlServer::revoke(const std::string& l) { std::lock_guard<std::mutex> g(p_->mu); return p_->book.revoke(l); }
std::vector<std::pair<std::string, std::string>> SwarmCtlServer::tokens() const { std::lock_guard<std::mutex> g(p_->mu); return p_->book.entries(); }

bool swarmPair(const std::string& host, int port, const std::string& typedCode, std::string& token, std::string& err) {
    std::string code = normalisePairingCode(typedCode);
    if (code.empty()) { err = "the pairing code must be 32 letters and digits (A-Z, 2-7)"; return false; }
    sock_t s = tcpConnect(host, port, 5000, err);
    if (s == kBadSock) return false;
    sockRecvTimeout(s, 5000);
    PairInitiator in;
    bool ok = false;
    std::string line = in.hello() + "\n", reply;
    if (sockSendAll(s, line.data(), line.size()) && readLine(s, reply, 300)) {
        std::string proof = in.handle(reply, code);
        if (!proof.empty()) {
            proof += "\n";
            if (sockSendAll(s, proof.data(), proof.size()) && readLine(s, reply, 400)) {
                in.handle(reply, code);
                ok = in.paired();
            }
        }
    }
    sockClose(s);
    if (!ok) { err = "pairing failed (wrong code, the code expired, or that node is not offering pairing)"; return false; }
    token = in.token();
    return true;
}

struct SwarmClient::Impl {
    sock_t s = kBadSock;
    std::unique_ptr<ControlSession> sess;
    int timeoutMs = 20000;
};
SwarmClient::SwarmClient() : p_(new Impl) {}
SwarmClient::~SwarmClient() { close(); }
void SwarmClient::close() { if (p_->s != kBadSock) { sockClose(p_->s); p_->s = kBadSock; } p_->sess.reset(); }
bool SwarmClient::connected() const { return p_->s != kBadSock && p_->sess; }
void SwarmClient::setTimeoutMs(int ms) { p_->timeoutMs = std::max(1000, ms); if (p_->s != kBadSock) sockRecvTimeout(p_->s, p_->timeoutMs); }
bool SwarmClient::connect(const std::string& host, int port, const std::string& token, std::string& err) {
    close();
    sock_t s = tcpConnect(host, port, 5000, err);
    if (s == kBadSock) return false;
    sockRecvTimeout(s, 5000);
    uint8_t ncb[16];
    systemRandom()(ncb, 16);
    std::string nc = hex(ncb, 16);
    std::string hello = "OACTL1 HELLO " + keyIdOf(token) + " " + nc + "\n", chal;
    if (!sockSendAll(s, hello.data(), hello.size()) || !readLine(s, chal, 100)) { err = "no answer from the node"; sockClose(s); return false; }
    if (chal.compare(0, 12, "OACTL1 CHAL ") != 0) { err = "the node does not know this pairing (pair again)"; sockClose(s); return false; }
    std::string ns = chal.substr(12);
    if (!isHex(ns, 32)) { err = "bad answer from the node"; sockClose(s); return false; }
    p_->sess.reset(new ControlSession(token, nc, ns, true));
    p_->s = s;
    sockRecvTimeout(s, p_->timeoutMs);
    return true;
}
bool SwarmClient::request(const std::string& plain, std::string& reply, std::string& err) {
    if (!connected()) { err = "not connected"; return false; }
    std::string f = p_->sess->seal(plain);
    if (f.empty()) { err = "request too large"; return false; }
    std::string out = frameWithLength(f);
    std::string frame;
    if (!sockSendAll(p_->s, out.data(), out.size()) || !readFrame(p_->s, frame)) { err = "the node did not answer"; close(); return false; }
    if (!p_->sess->open(frame, reply)) { err = "the answer failed its check (wrong key or tampered)"; close(); return false; }
    return true;
}

}   // namespace dect2::swarm
