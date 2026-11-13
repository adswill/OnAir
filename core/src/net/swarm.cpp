// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LAN swarm pure logic. See dect2/net/swarm.h.
#include "dect2/net/swarm.h"
#include "dect2/mesh/mesh_crypto.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>

namespace dect2::swarm {
namespace {

bool printable(const std::string& s, size_t maxLen, bool allowEmpty) {
    if (s.size() > maxLen || (!allowEmpty && s.empty())) return false;
    for (unsigned char c : s) if (c < 0x20 || c == 0x7f) return false;
    return true;
}
std::string clean(const std::string& s, size_t maxLen) {
    std::string o;
    for (unsigned char c : s) {
        if (o.size() >= maxLen) break;
        o.push_back((c < 0x20 || c == 0x7f || c == '=') ? ' ' : (char)c);
    }
    return o;
}
bool isHexLower16(const std::string& s) {
    if (s.size() != 16) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
bool parseNum(const std::string& s, double& v) {
    if (s.empty() || s.size() > 24) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == 'e' || c == 'E' || c == '+')) return false;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end && *end == 0 && std::isfinite(v);
}
std::string toHex(const uint8_t* p, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s.push_back(h[p[i] >> 4]); s.push_back(h[p[i] & 15]); }
    return s;
}
bool fromHex(const std::string& s, std::vector<uint8_t>& out, size_t wantBytes) {
    if (s.size() != wantBytes * 2) return false;
    out.clear();
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = v(s[i]), b = v(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back((uint8_t)(a * 16 + b));
    }
    return true;
}
bool ctEqual(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}
bool ctEqualStr(const std::string& a, const std::string& b) {
    size_t n = std::max(a.size(), b.size());
    uint8_t d = a.size() != b.size();
    for (size_t i = 0; i < n; i++) d |= (uint8_t)((i < a.size() ? a[i] : 0) ^ (i < b.size() ? b[i] : 0));
    return d == 0;
}
void hmac(const std::string& key, const std::string& msg, uint8_t out[32]) {
    meshcrypto::hmacSha256((const uint8_t*)key.data(), key.size(), (const uint8_t*)msg.data(), msg.size(), out);
}
bool validKind(const std::string& k) {
    if (k.empty() || k.size() > 16) return false;
    for (char c : k) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}
bool saneTime(int64_t t, int64_t now) { return t >= 0 && t >= now - 86400000LL && t <= now + 86400000LL; }
void addNode(std::vector<std::string>& v, const std::string& n) {
    if (v.size() < 16 && std::find(v.begin(), v.end(), n) == v.end()) v.push_back(n);
}
}   // namespace

// ---------------------------------------------------------------- announce
std::string encodeAnnounce(const Announce& a) {
    char b[64];
    std::string s = "OASWARM1\n";
    s += "id=" + a.id + "\n";
    s += "name=" + clean(a.name, 32) + "\n";
    s += "radio=" + clean(a.radio, 32) + "\n";
    s += "mode=" + clean(a.mode, 32) + "\n";
    s += "summary=" + clean(a.summary, 80) + "\n";
    snprintf(b, sizeof b, "lo=%.0f\nhi=%.0f\nload=%d\nport=%d\n", a.loHz, a.hiHz, a.load, a.ctlPort);
    return s + b;
}

bool parseAnnounce(const uint8_t* p, size_t n, Announce& out, std::string& err) {
    static const char* magic = "OASWARM1\n";
    if (!p || n < 10 || n > kMaxAnnounceBytes) { err = "size"; return false; }
    if (std::memchr(p, 0, n)) { err = "NUL byte"; return false; }
    std::string s((const char*)p, n);
    if (s.compare(0, 9, magic) != 0) { err = "magic"; return false; }
    if (s.back() != '\n') { err = "no final newline"; return false; }
    Announce a;
    std::set<std::string> seen;
    size_t pos = 9;
    while (pos < s.size()) {
        size_t e = s.find('\n', pos);
        std::string line = s.substr(pos, e - pos);
        pos = e + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) { err = "bad line"; return false; }
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (!seen.insert(k).second) { err = "duplicate key"; return false; }
        double d;
        if (k == "id") { if (!isHexLower16(v)) { err = "id"; return false; } a.id = v; }
        else if (k == "name") { if (!printable(v, 32, false)) { err = "name"; return false; } a.name = v; }
        else if (k == "radio") { if (!printable(v, 32, true)) { err = "radio"; return false; } a.radio = v; }
        else if (k == "mode") { if (!printable(v, 32, true)) { err = "mode"; return false; } a.mode = v; }
        else if (k == "summary") { if (!printable(v, 80, true)) { err = "summary"; return false; } a.summary = v; }
        else if (k == "lo") { if (!parseNum(v, d) || d < 0 || d > 1e12) { err = "lo"; return false; } a.loHz = d; }
        else if (k == "hi") { if (!parseNum(v, d) || d < 0 || d > 1e12) { err = "hi"; return false; } a.hiHz = d; }
        else if (k == "load") { if (!parseNum(v, d) || d < 0 || d > 100) { err = "load"; return false; } a.load = (int)d; }
        else if (k == "port") { if (!parseNum(v, d) || d < 0 || d > 65535) { err = "port"; return false; } a.ctlPort = (int)d; }
        else { err = "unknown key"; return false; }
    }
    if (a.id.empty() || a.name.empty() || !seen.count("lo") || !seen.count("hi")) { err = "missing field"; return false; }
    if (a.hiHz < a.loHz) { err = "range"; return false; }
    out = a;
    return true;
}

std::string announcePreview(const Announce& a) {
    char b[400];
    snprintf(b, sizeof b, "Name: %s\nRadio: %s\nTunable range: %.3f - %.3f MHz\nMode: %s\nLoad: %d%%\nSummary: %s\nControl port: %s\n"
                          "Sent as a small text packet to this local network only (never the internet). No IQ, no audio, no positions.",
             clean(a.name, 32).c_str(), clean(a.radio, 32).c_str(), a.loHz / 1e6, a.hiHz / 1e6, clean(a.mode, 32).c_str(), a.load,
             clean(a.summary, 80).c_str(), a.ctlPort ? "open for pairing" : "closed");
    return b;
}

bool isPrivateIpv4(uint32_t ip) {
    uint32_t a = ip >> 24, b = (ip >> 16) & 255;
    return a == 10 || a == 127 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254);
}
bool onLocalSubnet(uint32_t src, uint32_t local, uint32_t mask) {
    uint32_t inv = ~mask;
    if ((inv & (inv + 1)) != 0 || mask < 0xFF000000u) return false;   // not a contiguous prefix, or wider than /8
    if (src == 0 || src == 0xFFFFFFFFu || (src >> 28) == 0xE) return false;
    return (src & mask) == (local & mask);
}

// ---------------------------------------------------------------- node table
NodeTable::NodeTable(std::string ownId, int64_t expireMs, size_t maxNodes) : own_(std::move(ownId)), expireMs_(expireMs), max_(maxNodes) {}

NodeTable::Result NodeTable::update(const Announce& a, uint32_t src, uint32_t local, uint32_t mask, int64_t now) {
    if (!own_.empty() && a.id == own_) return SelfId;
    if (!isPrivateIpv4(src)) return BadSource;
    if (!onLocalSubnet(src, local, mask)) return WrongSubnet;
    auto li = lastFromIp_.find(src);
    if (li != lastFromIp_.end() && now - li->second < 1000 && now >= li->second) return RateLimited;
    if (lastFromIp_.size() > 512) expire(now);
    if (lastFromIp_.size() <= 512) lastFromIp_[src] = now;
    auto it = nodes_.find(a.id);
    if (it != nodes_.end()) {
        if (it->second.ip != src && now - it->second.lastSeenMs < expireMs_) return Spoofed;
    } else {
        if (nodes_.size() >= max_) return TableFull;
        int fromIp = 0;
        for (auto& kv : nodes_) if (kv.second.ip == src) fromIp++;
        if (fromIp >= 4) return TableFull;
    }
    Node& n = nodes_[a.id];
    n.info = a;
    n.ip = src;
    n.lastSeenMs = now;
    return Accepted;
}
void NodeTable::expire(int64_t now) {
    for (auto it = nodes_.begin(); it != nodes_.end();) it = (now - it->second.lastSeenMs > expireMs_) ? nodes_.erase(it) : std::next(it);
    for (auto it = lastFromIp_.begin(); it != lastFromIp_.end();) it = (now - it->second > 5000) ? lastFromIp_.erase(it) : std::next(it);
}
std::vector<Node> NodeTable::list() const {
    std::vector<Node> v;
    for (auto& kv : nodes_) v.push_back(kv.second);
    std::sort(v.begin(), v.end(), [](const Node& a, const Node& b) { return a.info.name != b.info.name ? a.info.name < b.info.name : a.info.id < b.info.id; });
    return v;
}

// ---------------------------------------------------------------- merged messages
std::string MessageMerger::normalise(const std::string& t) {
    std::string o;
    bool sp = false;
    for (unsigned char c : t) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { sp = !o.empty(); continue; }
        if (sp) { o.push_back(' '); sp = false; }
        o.push_back((c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c);
    }
    return o;
}
MessageMerger::Result MessageMerger::add(const Msg& m, int64_t now) {
    if (!validKind(m.kind) || m.text.empty() || m.text.size() > 512 || !printable(m.nodeId, 32, false) || !saneTime(m.timeMs, now)) return Rejected;
    for (unsigned char c : m.text) if (c == 0) return Rejected;
    std::string norm = normalise(m.text);
    if (norm.empty()) return Rejected;
    std::string key = m.kind + '\x1f' + norm;
    auto it = latest_.find(key);
    if (it != latest_.end() && it->second >= base_ && it->second - base_ < rows_.size()) {
        MergedRow& r = rows_[it->second - base_];
        int64_t d = m.timeMs - r.timeMs;
        if (d < 0) d = -d;
        if (d <= window_) { addNode(r.nodes, m.nodeId); return Duplicate; }
    }
    MergedRow r;
    r.kind = m.kind;
    r.text = m.text;
    r.timeMs = m.timeMs;
    r.nodes.push_back(m.nodeId);
    rows_.push_back(std::move(r));
    latest_[key] = base_ + rows_.size() - 1;
    while (rows_.size() > max_) { rows_.pop_front(); base_++; }
    if (latest_.size() > 2 * max_ + 16) {
        for (auto i = latest_.begin(); i != latest_.end();) i = (i->second < base_) ? latest_.erase(i) : std::next(i);
    }
    return New;
}

// ---------------------------------------------------------------- track board
bool TrackBoard::update(const std::string& kind, const std::string& id, double lat, double lon, const std::string& nodeId, int64_t t, int64_t now) {
    if (!validKind(kind) || id.empty() || id.size() > 16 || !printable(nodeId, 32, false) || !saneTime(t, now)) return false;
    for (unsigned char c : id) if (!std::isalnum(c)) return false;
    if (!std::isfinite(lat) || !std::isfinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180 || (lat == 0 && lon == 0)) return false;
    std::string key = kind + ":" + id;
    auto it = t_.find(key);
    if (it == t_.end()) {
        if (t_.size() >= max_) return false;
        Track tr;
        tr.kind = kind; tr.id = id; tr.nodeId = nodeId; tr.lat = lat; tr.lon = lon; tr.timeMs = t;
        tr.nodes.push_back(nodeId);
        t_[key] = tr;
        return true;
    }
    Track& tr = it->second;
    addNode(tr.nodes, nodeId);
    if (t > tr.timeMs) { tr.lat = lat; tr.lon = lon; tr.timeMs = t; tr.nodeId = nodeId; }
    return true;
}
void TrackBoard::expire(int64_t now) {
    for (auto it = t_.begin(); it != t_.end();) it = (now - it->second.timeMs > keep_) ? t_.erase(it) : std::next(it);
}
std::vector<Track> TrackBoard::list() const {
    std::vector<Track> v;
    for (auto& kv : t_) v.push_back(kv.second);
    return v;
}

// ---------------------------------------------------------------- coordinated scanning
bool splitRange(double lo, double hi, const std::vector<Cap>& caps, double step, SplitResult& out, std::string& err) {
    out = SplitResult();
    if (!std::isfinite(lo) || !std::isfinite(hi) || !std::isfinite(step) || lo < 0 || hi <= lo || hi > 1e12 || step <= 0) { err = "bad range or step"; return false; }
    if (caps.empty() || caps.size() > 64) { err = "need 1..64 nodes"; return false; }
    std::set<std::string> ids;
    for (auto& c : caps) {
        if (c.id.empty() || !ids.insert(c.id).second || !std::isfinite(c.minHz) || !std::isfinite(c.maxHz) || !std::isfinite(c.bwHz) ||
            c.minHz < 0 || c.maxHz <= c.minHz || c.bwHz <= 0 || c.load < 0 || c.load > 100) { err = "bad node capability"; return false; }
    }
    std::vector<Cap> cs = caps;
    std::sort(cs.begin(), cs.end(), [](const Cap& a, const Cap& b) { return a.id < b.id; });
    std::vector<double> assigned(cs.size(), 0);
    double cursor = lo;
    for (int guard = 0; cursor < hi && guard < 200000; guard++) {
        int best = -1;
        double bestScore = 0;
        for (size_t i = 0; i < cs.size(); i++) {
            if (cs[i].minHz > cursor || cs[i].maxHz <= cursor) continue;
            double w = cs[i].bwHz * (101.0 - cs[i].load) / 101.0;
            double score = assigned[i] / w;
            if (best < 0 || score < bestScore) { best = (int)i; bestScore = score; }
        }
        if (best < 0) {
            double next = hi;
            for (auto& c : cs) if (c.minHz > cursor && c.minHz < next) next = c.minHz;
            out.uncovered.push_back({cursor, next});
            cursor = next;
            continue;
        }
        double chunk = std::floor(cs[best].bwHz / step) * step;
        if (chunk < step) chunk = step;
        double end = std::min({cursor + chunk, hi, cs[best].maxHz});
        if (!(end > cursor)) { err = "no progress"; return false; }
        if (!out.slices.empty() && out.slices.back().id == cs[best].id && out.slices.back().hiHz == cursor) out.slices.back().hiHz = end;
        else out.slices.push_back({cs[best].id, cursor, end});
        assigned[best] += end - cursor;
        cursor = end;
    }
    if (cursor < hi) { err = "range needs too many slices"; return false; }
    return true;
}

std::vector<MergedHit> mergeHits(const std::vector<Hit>& hits, double tol) {
    std::vector<Hit> v;
    for (auto& h : hits) if (std::isfinite(h.freqHz) && std::isfinite(h.level) && h.freqHz > 0 && printable(h.nodeId, 32, false)) v.push_back(h);
    std::sort(v.begin(), v.end(), [](const Hit& a, const Hit& b) { return a.freqHz < b.freqHz; });
    if (!(tol >= 0) || !std::isfinite(tol)) tol = 0;
    std::vector<MergedHit> out;
    double lastF = 0;
    for (auto& h : v) {
        if (out.empty() || h.freqHz - lastF > tol) {
            MergedHit m;
            m.freqHz = h.freqHz; m.level = h.level; m.nodes.push_back(h.nodeId);
            out.push_back(m);
        } else {
            MergedHit& m = out.back();
            if (h.level > m.level) { m.level = h.level; m.freqHz = h.freqHz; }
            addNode(m.nodes, h.nodeId);
        }
        lastF = h.freqHz;
    }
    return out;
}

// ---------------------------------------------------------------- pairing
RandFn systemRandom() {
    return [](uint8_t* p, size_t n) {
        std::random_device rd;
        for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(rd() & 255);
    };
}
std::string newPairingCode(const RandFn& rnd) {
    static const char* b32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    uint8_t b[20];
    rnd(b, 20);
    std::string s;
    uint32_t acc = 0;
    int bits = 0;
    for (uint8_t x : b) {
        acc = (acc << 8) | x; bits += 8;
        while (bits >= 5) { s.push_back(b32[(acc >> (bits - 5)) & 31]); bits -= 5; }
    }
    return s;   // 160 bits = exactly 32 characters
}
std::string normalisePairingCode(const std::string& typed) {
    std::string o;
    for (unsigned char c : typed) {
        if (c == ' ' || c == '-') continue;
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
        if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7'))) return std::string();
        o.push_back((char)c);
        if (o.size() > 32) return std::string();
    }
    return o.size() == 32 ? o : std::string();
}

namespace {
const char* kProto = "OAPAIR1 ";
std::string err(const char* why) { return std::string(kProto) + "ERR " + why; }
bool splitLine(const std::string& line, std::vector<std::string>& w) {
    w.clear();
    if (line.size() > 200) return false;
    std::string cur;
    for (unsigned char c : line) {
        if (c == ' ') { if (cur.empty()) return false; w.push_back(cur); cur.clear(); }
        else if (c < 0x21 || c > 0x7e) return false;
        else cur.push_back((char)c);
    }
    if (cur.empty()) return false;
    w.push_back(cur);
    return w.size() <= 4 && w[0] == "OAPAIR1";
}
std::string proofC(const std::string& code, const std::string& nc, const std::string& ns) {
    uint8_t o[32];
    hmac(code, "C" + nc + ns, o);
    return toHex(o, 32);
}
}   // namespace

bool PairResponder::offer(const std::string& code, int64_t now, int64_t ttl) {
    if (code.size() != 32 || normalisePairingCode(code) != code) return false;   // an empty string normalises to itself: it must never be an offer
    st_ = Offered; code_ = code; expires_ = now + ttl; fails_ = 0;
    peer_.clear(); nc_.clear(); ns_.clear();
    return true;
}
void PairResponder::cancel() { st_ = Idle; code_.clear(); peer_.clear(); nc_.clear(); ns_.clear(); }
PairResponder::State PairResponder::state(int64_t now) {
    if ((st_ == Offered || st_ == Challenged) && now >= expires_) cancel();
    if (st_ == Challenged && now - challengeAt_ > 10000) { st_ = Offered; peer_.clear(); nc_.clear(); ns_.clear(); }
    return st_;
}
bool PairResponder::takePaired(std::string& token, std::string& peer) {
    if (taken_) return false;
    taken_ = true; token = token_; peer = donePeer_;
    token_.clear();
    return true;
}
std::string PairResponder::handle(const std::string& line, const std::string& peerKey, int64_t now) {
    State s = state(now);
    if (s == Idle || s == Done) return err("not pairing");
    if (s == Locked) return err("locked");
    std::vector<std::string> w;
    if (!splitLine(line, w) || w.size() != 3) return err("syntax");
    std::vector<uint8_t> raw;
    if (w[1] == "HELLO") {
        if (s == Challenged && peerKey != peer_) return err("busy");
        if (!fromHex(w[2], raw, 16)) return err("syntax");
        nc_ = w[2];
        uint8_t r[16];
        rnd_(r, 16);
        ns_ = toHex(r, 16);
        peer_ = peerKey; st_ = Challenged; challengeAt_ = now;
        return std::string(kProto) + "CHAL " + ns_;
    }
    if (w[1] == "PROOF") {
        if (s != Challenged || peerKey != peer_) return err("no challenge");
        std::vector<uint8_t> got;
        if (!fromHex(w[2], got, 32)) return err("syntax");
        uint8_t want[32];
        hmac(code_, "C" + nc_ + ns_, want);
        if (!ctEqual(got.data(), want, 32)) {
            fails_++;
            nc_.clear(); ns_.clear(); peer_.clear();
            if (fails_ >= 3) { st_ = Locked; code_.clear(); } else st_ = Offered;
            return err("bad proof");
        }
        uint8_t r[12];
        rnd_(r, 12);
        token_ = toHex(r, 12);   // 24 hex characters
        uint8_t ks[32], sp[32];
        hmac(code_, "T" + nc_ + ns_, ks);
        hmac(code_, "S" + nc_ + ns_ + token_, sp);
        uint8_t wrapped[24];
        for (int i = 0; i < 24; i++) wrapped[i] = (uint8_t)token_[i] ^ ks[i];
        std::string reply = std::string(kProto) + "OK " + toHex(sp, 32) + " " + toHex(wrapped, 24);
        donePeer_ = peerKey; taken_ = false;
        code_.clear(); nc_.clear(); ns_.clear(); peer_.clear();
        st_ = Done;
        return reply;
    }
    return err("syntax");
}

std::string PairInitiator::hello() {
    uint8_t r[16];
    rnd_(r, 16);
    nc_ = toHex(r, 16);
    ns_.clear(); ok_ = failed_ = sentProof_ = false;
    return std::string(kProto) + "HELLO " + nc_;
}
std::string PairInitiator::handle(const std::string& line, const std::string& code) {
    std::vector<std::string> w;
    if (nc_.empty() || ok_ || failed_ || !splitLine(line, w) || w.size() < 2) { if (!ok_) failed_ = true; return std::string(); }
    std::vector<uint8_t> raw;
    if (w[1] == "CHAL" && w.size() == 3 && !sentProof_ && fromHex(w[2], raw, 16)) {
        ns_ = w[2]; sentProof_ = true;
        return std::string(kProto) + "PROOF " + proofC(code, nc_, ns_);
    }
    if (w[1] == "OK" && w.size() == 4 && sentProof_) {
        std::vector<uint8_t> sp, wr;
        if (!fromHex(w[2], sp, 32) || !fromHex(w[3], wr, 24)) { failed_ = true; return std::string(); }
        uint8_t ks[32];
        hmac(code, "T" + nc_ + ns_, ks);
        std::string tok;
        for (int i = 0; i < 24; i++) tok.push_back((char)(wr[i] ^ ks[i]));
        uint8_t want[32];
        hmac(code, "S" + nc_ + ns_ + tok, want);
        if (!ctEqual(sp.data(), want, 32)) { failed_ = true; return std::string(); }   // the other side did not know the code
        for (char c : tok) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { failed_ = true; return std::string(); }
        token_ = tok; ok_ = true;
        return std::string();
    }
    failed_ = true;
    return std::string();
}

void TokenBook::add(const std::string& token, const std::string& label) {
    if (token.size() < 16 || label.empty()) return;
    for (auto& e : t_) if (e.second == label) { e.first = token; return; }
    if (t_.size() < 32) t_.push_back({token, label});
}
bool TokenBook::revoke(const std::string& label) {
    for (auto it = t_.begin(); it != t_.end(); ++it) if (it->second == label) { t_.erase(it); return true; }
    return false;
}
bool TokenBook::authorise(const std::string& token, std::string* label) const {
    bool ok = false;
    for (auto& e : t_) if (ctEqualStr(e.first, token) && token.size() >= 16 && !ok) { ok = true; if (label) *label = e.second; }
    return ok;
}
std::string keyIdOf(const std::string& token) {
    uint8_t o[32];
    hmac(token, "OACTL1 id", o);
    return toHex(o, 8);
}
bool TokenBook::findByKeyId(const std::string& keyId, std::string& token, std::string* label) const {
    bool ok = false;
    for (auto& e : t_) if (!ok && ctEqualStr(keyIdOf(e.first), keyId)) { ok = true; token = e.first; if (label) *label = e.second; }
    return ok;
}
bool controlAllowed(const std::string& c) {
    static const char* ok[] = {"status", "start", "stop", "set_frequency", "set_mode", "set_gain", "set_rate", "scan_slice"};
    for (const char* o : ok) if (c == o) return true;
    return false;
}

bool RateLimiter::allow(int64_t now) {
    if (!started_) { started_ = true; last_ = now; }
    double dt = (double)(now - last_);
    if (dt < 0) dt = 0;
    last_ = std::max(last_, now);
    tokens_ = std::min(burst_, tokens_ + dt * rate_ / 1000.0);
    if (tokens_ >= 1) { tokens_ -= 1; return true; }
    return false;
}

// ---------------------------------------------------------------- memory sync
bool memValid(const Mem& m) {
    if (m.key.empty() || m.key.size() > 48) return false;
    for (unsigned char c : m.key) if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
    return printable(m.name, 64, true) && printable(m.mode, 32, true) && printable(m.note, 200, true) && std::isfinite(m.freqHz) && m.freqHz >= 0 &&
           m.freqHz <= 1e12 && m.modifiedMs >= 0;
}
MemMergeStats mergeMemories(std::vector<Mem>& local, const std::vector<Mem>& remote) {
    MemMergeStats st;
    std::map<std::string, size_t> idx;
    for (size_t i = 0; i < local.size(); i++) idx[local[i].key] = i;
    for (auto& r : remote) {
        if (!memValid(r)) { st.rejected++; continue; }
        auto it = idx.find(r.key);
        if (it == idx.end()) {
            if (local.size() >= 10000) { st.rejected++; continue; }
            idx[r.key] = local.size();
            local.push_back(r);
            st.added++;
        } else if (r.modifiedMs > local[it->second].modifiedMs) {
            bool wasLive = !local[it->second].deleted;
            local[it->second] = r;
            if (r.deleted && wasLive) st.removed++; else st.updated++;
        } else st.kept++;
    }
    return st;
}

} // namespace dect2::swarm
