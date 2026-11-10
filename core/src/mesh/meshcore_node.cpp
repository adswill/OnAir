// MeshCore node: see meshcore_node.h. Each step names the firmware function it follows (meshcore-dev/MeshCore, MIT licence).
#include "dect2/mesh/meshcore_node.h"
#include "dect2/mesh/mesh_crypto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <sstream>

namespace dect2 {
namespace meshcore {

using namespace meshcrypto;

namespace {

void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i))); }
uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// the firmware's test client key pair (LocalIdentity::validatePrivateKey)
const uint8_t kTestPrv[64] = {0x70, 0x65, 0xe1, 0x8f, 0xd9, 0xfa, 0xbb, 0x70, 0xc1, 0xed, 0x90, 0xdc, 0xa1, 0x99, 0x07, 0xde,
                              0x69, 0x8c, 0x88, 0xb7, 0x09, 0xea, 0x14, 0x6e, 0xaf, 0xd9, 0x3d, 0x9b, 0x83, 0x0c, 0x7b, 0x60,
                              0xc4, 0x68, 0x11, 0x93, 0xc7, 0x9b, 0xbc, 0x39, 0x94, 0x5b, 0xa8, 0x06, 0x41, 0x04, 0xbb, 0x61,
                              0x8f, 0x8f, 0xd7, 0xa8, 0x4a, 0x0a, 0xf6, 0xf5, 0x70, 0x33, 0xd6, 0xe8, 0xdd, 0xcd, 0x64, 0x71};
const uint8_t kTestPub[32] = {0x1e, 0xc7, 0x71, 0x75, 0xb0, 0x91, 0x8e, 0xd2, 0x06, 0xf9, 0xae, 0x04, 0xec, 0x13, 0x6d, 0x6d,
                              0x5d, 0x43, 0x15, 0xbb, 0x26, 0x30, 0x54, 0x27, 0xf6, 0x45, 0xb4, 0x92, 0xe9, 0x35, 0x0c, 0x10};
const uint8_t kPublicKey[16] = {0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a, 0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};

// percent-encoding of the characters that would break a saved line
std::string enc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c <= ' ' || c == '%' || c == 0x7f) { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; }
        else o += (char)c;
    }
    return o.empty() ? "%00" : o;
}
std::string dec(const std::string& s) {
    if (s == "%00") return "";
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) { o += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
        else o += s[i];
    }
    return o;
}

// the longest prefix of s (at most max bytes) that ends on a UTF-8 character boundary
size_t utf8Prefix(const std::string& s, size_t max) {
    if (s.size() <= max) return s.size();
    size_t n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return n;
}

} // namespace

// ---------------------------------------------------------------- packets

bool validPathLen(uint8_t pathLen) {
    const int count = pathLen & 63, size = (pathLen >> 6) + 1;
    if (size == 4) return false;
    return count * size <= (int)kMaxPath;
}

std::vector<uint8_t> Packet::bytes() const {
    std::vector<uint8_t> o;
    o.push_back(header);
    if (hasTransport()) {
        o.push_back((uint8_t)transport[0]); o.push_back((uint8_t)(transport[0] >> 8));
        o.push_back((uint8_t)transport[1]); o.push_back((uint8_t)(transport[1] >> 8));
    }
    o.push_back(pathLen);
    const size_t pl = (size_t)hashCount() * (size_t)hashSize();
    for (size_t i = 0; i < pl; i++) o.push_back(i < path.size() ? path[i] : 0);
    o.insert(o.end(), payload.begin(), payload.end());
    return o;
}

void Packet::packetHash(uint8_t out[8]) const {
    Sha256 h;
    const uint8_t t = (uint8_t)type();
    h.update(&t, 1);
    if (type() == TTrace) { const uint8_t pl[2] = {pathLen, 0}; h.update(pl, 2); }   // the firmware hashes its uint16 path_len
    if (!payload.empty()) h.update(payload.data(), payload.size());
    uint8_t d[32];
    h.final(d);
    std::memcpy(out, d, 8);
}

bool parsePacket(const uint8_t* b, size_t n, Packet& out) {
    out = Packet();
    if (n < 2) return false;
    size_t i = 0;
    out.header = b[i++];
    if (out.hasTransport()) {
        if (n < 6) return false;
        out.transport[0] = (uint16_t)(b[1] | b[2] << 8);
        out.transport[1] = (uint16_t)(b[3] | b[4] << 8);
        i = 5;
    }
    if (i >= n) return false;
    out.pathLen = b[i++];
    if (!validPathLen(out.pathLen)) return false;
    const size_t bl = (size_t)out.hashCount() * (size_t)out.hashSize();
    if (i + bl > n) return false;
    out.path.assign(b + i, b + i + bl);
    i += bl;
    if (i >= n) return false;                                   // readFrom: no payload is a bad encoding
    if (n - i > kMaxPayload) return false;
    out.payload.assign(b + i, b + n);
    return true;
}

void floodOf(Packet& p, int hashSize) {
    hashSize = std::clamp(hashSize, 1, 3);
    p.setRoute(RouteFlood);
    p.pathLen = (uint8_t)((hashSize - 1) << 6);
    p.path.clear();
}

void directOf(Packet& p, uint8_t pathLen, const std::vector<uint8_t>& path) {
    p.setRoute(RouteDirect);
    p.pathLen = pathLen;
    const size_t bl = (size_t)(pathLen & 63) * (size_t)((pathLen >> 6) + 1);
    p.path.assign(path.begin(), path.begin() + std::min(bl, path.size()));
    p.path.resize(bl, 0);
}

// ---------------------------------------------------------------- crypto

std::vector<uint8_t> encryptThenMac(const uint8_t secret[32], const uint8_t* data, size_t n) {
    const std::vector<uint8_t> ct = aesEcbEncrypt(secret, 16, data, n);   // zero padded
    uint8_t mac[32];
    hmacSha256(secret, 32, ct.data(), ct.size(), mac);
    std::vector<uint8_t> o{mac[0], mac[1]};
    o.insert(o.end(), ct.begin(), ct.end());
    return o;
}

bool macThenDecrypt(const uint8_t secret[32], const uint8_t* src, size_t n, std::vector<uint8_t>& plain) {
    plain.clear();
    if (n <= 2) return false;
    const size_t encLen = n - 2;
    if (encLen % 16 != 0) return false;
    uint8_t mac[32];
    hmacSha256(secret, 32, src + 2, encLen, mac);
    if (mac[0] != src[0] || mac[1] != src[1]) return false;
    plain = aesEcbDecrypt(secret, 16, src + 2, encLen);
    return !plain.empty();
}

std::string hexOf(const uint8_t* p, size_t n) { return hexEncode(p, n, false); }

void systemRandom(uint8_t* p, size_t n) {
    std::random_device rd;
    for (size_t i = 0; i < n; i += 4) {
        const uint32_t v = rd();
        for (size_t k = 0; k < 4 && i + k < n; k++) p[i + k] = (uint8_t)(v >> (8 * k));
    }
}

bool identityFromSeed(const uint8_t seed[32], Identity& out) {
    out = Identity();
    ed25519ExpandSeed(seed, out.prv);
    ed25519PublicFromPrivate(out.prv, out.pub);
    out.valid = out.pub[0] != 0x00 && out.pub[0] != 0xFF;
    return out.valid;
}

Identity identityNew() {
    Identity id;
    for (int i = 0; i < 64; i++) {
        uint8_t seed[32];
        systemRandom(seed, 32);
        const bool ok = identityFromSeed(seed, id);
        std::memset(seed, 0, sizeof seed);
        if (ok) break;
    }
    return id;
}

bool identityFromPrivateHex(const std::string& hex, Identity& out, std::string* why) {
    out = Identity();
    std::vector<uint8_t> raw;
    if (!hexDecode(hex, raw) || (raw.size() != 64 && raw.size() != 96)) { if (why) *why = "a MeshCore private key is 64 bytes (128 hex digits)"; return false; }
    std::memcpy(out.prv, raw.data(), 64);
    ed25519PublicFromPrivate(out.prv, out.pub);
    if (raw.size() == 96 && std::memcmp(out.pub, raw.data() + 64, 32) != 0) { if (why) *why = "the public key after the private key does not belong to it"; out = Identity(); return false; }
    if (out.pub[0] == 0x00 || out.pub[0] == 0xFF) { if (why) *why = "MeshCore does not use keys whose public key starts with 00 or FF"; out = Identity(); return false; }
    uint8_t s1[32], s2[32];
    ed25519KeyExchange(s1, kTestPub, out.prv);
    ed25519KeyExchange(s2, out.pub, kTestPrv);
    bool nz = false;
    for (int i = 0; i < 32; i++) nz |= s1[i] != 0;
    if (std::memcmp(s1, s2, 32) != 0 || !nz) { if (why) *why = "this private key fails the firmware's key check"; out = Identity(); return false; }
    out.valid = true;
    return true;
}

std::string identityPrivateHex(const Identity& id) { return hexOf(id.prv, 64); }

void sharedSecret(const Identity& self, const uint8_t otherPub[32], uint8_t out[32]) { ed25519KeyExchange(out, otherPub, self.prv); }

// ---------------------------------------------------------------- channels

Channel publicChannel() {
    Channel c;
    c.name = "Public";
    std::memcpy(c.secret, kPublicKey, 16);
    c.keyLen = 16;
    uint8_t h[32];
    sha256(c.secret, 16, h);
    c.hash = h[0];
    return c;
}

bool makeChannel(const std::string& nameIn, const std::string& keyIn, Channel& out, std::string* why) {
    out = Channel();
    std::string name = nameIn, key = keyIn;
    while (!name.empty() && (name.back() == ' ')) name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(0, 1);
    std::vector<uint8_t> raw;
    if (key.empty() && !name.empty() && name[0] == '#') {   // hashtag channel: anyone who knows the name has the key
        uint8_t h[32];
        sha256((const uint8_t*)name.data(), name.size(), h);
        raw.assign(h, h + 16);
    } else {
        std::string k;
        for (char c : key) if (c != ' ') k += c;
        if (!((k.size() == 32 || k.size() == 64) && hexDecode(k, raw))) { raw.clear(); if (!base64Decode(k, raw)) raw.clear(); }
        if (raw.size() != 16 && raw.size() != 32) { if (why) *why = "the key must be 16 or 32 bytes (32 or 64 hex digits, or base64); a name that starts with # needs no key"; return false; }
    }
    if (name.empty()) { if (why) *why = "the channel needs a name"; return false; }
    out.name = name;
    out.keyLen = (int)raw.size();
    std::memcpy(out.secret, raw.data(), raw.size());
    uint8_t h[32];
    sha256(out.secret, (size_t)out.keyLen, h);
    out.hash = h[0];
    return true;
}

std::string channelKeyHex(const Channel& c) { return hexOf(c.secret, (size_t)c.keyLen); }

// ---------------------------------------------------------------- advert data

std::vector<uint8_t> encodeAdvertData(const AdvertData& a) {
    std::vector<uint8_t> o;
    o.push_back((uint8_t)(a.type & 0x0F));
    if (a.hasPos) {
        o[0] |= 0x10;
        put32(o, (uint32_t)(int32_t)std::lround(a.lat * 1e6));
        put32(o, (uint32_t)(int32_t)std::lround(a.lon * 1e6));
    }
    if (a.feat1) { o[0] |= 0x20; o.push_back((uint8_t)a.feat1); o.push_back((uint8_t)(a.feat1 >> 8)); }
    if (a.feat2) { o[0] |= 0x40; o.push_back((uint8_t)a.feat2); o.push_back((uint8_t)(a.feat2 >> 8)); }
    if (!a.name.empty()) {
        const size_t nl = utf8Prefix(a.name, kMaxAdvertData - o.size());
        if (nl > 0) { o[0] |= 0x80; o.insert(o.end(), a.name.begin(), a.name.begin() + (long)nl); }
    }
    return o;
}

bool parseAdvertData(const uint8_t* p, size_t n, AdvertData& out) {
    out = AdvertData();
    if (n < 1) return false;
    const uint8_t f = p[0];
    out.type = f & 0x0F;
    size_t i = 1;
    if (f & 0x10) {
        if (i + 8 > n) return false;
        out.hasPos = true;
        out.lat = (int32_t)get32(p + i) / 1e6; out.lon = (int32_t)get32(p + i + 4) / 1e6;
        i += 8;
    }
    if (f & 0x20) { if (i + 2 > n) return false; out.feat1 = (uint16_t)(p[i] | p[i + 1] << 8); i += 2; }
    if (f & 0x40) { if (i + 2 > n) return false; out.feat2 = (uint16_t)(p[i] | p[i + 1] << 8); i += 2; }
    if (f & 0x80) {
        std::string s((const char*)p + i, n - i);
        const size_t z = s.find('\0');
        if (z != std::string::npos) s.resize(z);
        out.name = s;
    }
    return true;
}

// ---------------------------------------------------------------- builders

Packet buildAdvert(const Identity& id, uint32_t t, const AdvertData& app) {
    const std::vector<uint8_t> ad = encodeAdvertData(app);
    std::vector<uint8_t> msg(id.pub, id.pub + 32);
    put32(msg, t);
    msg.insert(msg.end(), ad.begin(), ad.end());
    uint8_t sig[64];
    ed25519SignPrivate(id.prv, id.pub, msg.data(), msg.size(), sig);
    Packet p;
    p.header = (uint8_t)(TAdvert << 2);
    p.payload.assign(id.pub, id.pub + 32);
    put32(p.payload, t);
    p.payload.insert(p.payload.end(), sig, sig + 64);
    p.payload.insert(p.payload.end(), ad.begin(), ad.end());
    return p;
}

bool buildText(const Identity& self, const uint8_t destPub[32], const uint8_t secret[32], uint32_t t, int attempt, const std::string& textIn,
               Packet& out, uint32_t& expectedAck) {
    std::string text = textIn.substr(0, utf8Prefix(textIn, kMaxText));
    if (attempt > 3 && text.size() > kMaxText - 2) text.resize(utf8Prefix(text, kMaxText - 2));
    std::vector<uint8_t> d;
    put32(d, t);
    d.push_back((uint8_t)(attempt & 3));                        // TXT_TYPE_PLAIN << 2 | attempt
    d.insert(d.end(), text.begin(), text.end());
    {   // BaseChatMesh::composeMsgPacket: SHA-256 of timestamp, flags and text, then the sender's public key
        Sha256 h;
        h.update(d.data(), d.size());
        h.update(self.pub, 32);
        uint8_t dg[32];
        h.final(dg);
        expectedAck = get32(dg);
    }
    if (attempt > 3) { d.push_back(0); d.push_back((uint8_t)attempt); }
    if (d.size() + 2 + 15 > kMaxPayload) return false;
    out = Packet();
    out.header = (uint8_t)(TTxtMsg << 2);
    out.payload.push_back(destPub[0]);
    out.payload.push_back(self.pub[0]);
    const std::vector<uint8_t> em = encryptThenMac(secret, d.data(), d.size());
    out.payload.insert(out.payload.end(), em.begin(), em.end());
    return true;
}

Packet buildGroupText(const Channel& c, uint32_t t, const std::string& sender, const std::string& textIn) {
    // BaseChatMesh::sendGroupMessage: "<sender>: <text>", cut to MAX_TEXT_LEN
    const std::string prefix = sender + ": ";
    std::string text = textIn;
    if (prefix.size() + text.size() > kMaxText) text.resize(utf8Prefix(text, prefix.size() < kMaxText ? kMaxText - prefix.size() : 0));
    std::vector<uint8_t> d;
    put32(d, t);
    d.push_back(0);
    d.insert(d.end(), prefix.begin(), prefix.end());
    d.insert(d.end(), text.begin(), text.end());
    Packet p;
    p.header = (uint8_t)(TGrpTxt << 2);
    p.payload.push_back(c.hash);
    const std::vector<uint8_t> em = encryptThenMac(c.secret, d.data(), d.size());
    p.payload.insert(p.payload.end(), em.begin(), em.end());
    return p;
}

Packet buildAck(const uint8_t* ack, size_t n) {
    Packet p;
    p.header = (uint8_t)(TAck << 2);
    p.payload.assign(ack, ack + n);
    return p;
}

Packet buildPathReturn(uint8_t destHash, uint8_t srcHash, const uint8_t secret[32], uint8_t pathLen, const std::vector<uint8_t>& path,
                       int extraType, const std::vector<uint8_t>& extra, const uint8_t rnd4[4]) {
    std::vector<uint8_t> d;
    d.push_back(pathLen);
    const size_t bl = (size_t)(pathLen & 63) * (size_t)((pathLen >> 6) + 1);
    for (size_t i = 0; i < bl; i++) d.push_back(i < path.size() ? path[i] : 0);
    if (!extra.empty()) { d.push_back((uint8_t)extraType); d.insert(d.end(), extra.begin(), extra.end()); }
    else { d.push_back(0xFF); d.insert(d.end(), rnd4, rnd4 + 4); }   // createPathReturn: a dummy type and 4 random bytes
    Packet p;
    p.header = (uint8_t)(TPath << 2);
    p.payload.push_back(destHash);
    p.payload.push_back(srcHash);
    const std::vector<uint8_t> em = encryptThenMac(secret, d.data(), d.size());
    p.payload.insert(p.payload.end(), em.begin(), em.end());
    return p;
}

// ---------------------------------------------------------------- airtime, duty cycle

double airtimeSec(const Radio& r, size_t n) {
    const int sf = std::clamp(r.sf, 5, 12);
    const double bw = r.bwHz > 0 ? r.bwHz : 125000;
    const double ts = std::ldexp(1.0, sf) / bw;
    const int de = ts > 0.016 ? 1 : 0;                         // low data rate optimisation
    const int cr = std::clamp(r.cr, 5, 8) - 4;
    const double num = 8.0 * (double)n - 4.0 * sf + 28 + (r.crc ? 16 : 0);
    const double den = 4.0 * (sf - 2 * de);
    const double sym = 8 + std::max(std::ceil(num / den) * (cr + 4), 0.0);
    return (r.preamble + 4.25) * ts + sym * ts;
}

DutyRule dutyCycleRule(double f) {
    const double m = f / 1e6;
    struct R { double lo, hi, lim; const char* name; };
    static const R t[] = {
        {869.400, 869.650, 0.10, "869.40 - 869.65 MHz (ERC 70-03 h1.7, 10 %)"},
        {868.000, 868.600, 0.01, "868.0 - 868.6 MHz (ERC 70-03 h1.5, 1 %)"},
        {868.700, 869.200, 0.001, "868.7 - 869.2 MHz (ERC 70-03 h1.6, 0.1 %)"},
        {869.700, 870.000, 0.01, "869.7 - 870.0 MHz (ERC 70-03 h1.9, 1 %)"},
        {865.000, 868.000, 0.01, "865 - 868 MHz (ERC 70-03 h1.4, 1 %)"},
        {863.000, 865.000, 0.001, "863 - 865 MHz (ERC 70-03 h1.3, 0.1 %)"},
        {868.600, 868.700, 0.01, "868.6 - 868.7 MHz (1 %)"},
        {869.200, 869.400, 0.001, "869.2 - 869.4 MHz (0.1 %)"},
        {433.050, 434.790, 0.10, "433.05 - 434.79 MHz (ERC 70-03 h1.2, 10 %)"},
    };
    for (const R& r : t) if (m >= r.lo && m <= r.hi) return {r.name, r.lim};
    return {"", 0};
}

// ---------------------------------------------------------------- node

Node::Node() {
    unixClock = [] { return (uint32_t)std::time(nullptr); };
    random = systemRandom;
    channels_.push_back(publicChannel());
}

void Node::setIdentity(const Identity& id) { id_ = id; }

uint32_t Node::uniqueTime() {
    const uint32_t t = unixClock ? unixClock() : (uint32_t)std::time(nullptr);
    if (t <= lastUnique_) return ++lastUnique_;
    return lastUnique_ = t;
}

Contact* Node::findContact(const uint8_t pub[32]) {
    for (auto& c : contacts_) if (std::memcmp(c.pub, pub, 32) == 0) return &c;
    return nullptr;
}
Contact* Node::findContactHex(const std::string& hex) {
    std::vector<uint8_t> raw;
    if (!hexDecode(hex, raw) || raw.size() != 32) return nullptr;
    return findContact(raw.data());
}
Message* Node::findMsg(uint32_t id) {
    for (auto& m : msgs_) if (m.outgoing && m.id == id) return &m;
    return nullptr;
}

bool Node::wasSeen(const Packet& p) const {
    uint8_t h[8];
    p.packetHash(h);
    for (const auto& e : seen_) if (std::memcmp(e, h, 8) == 0) return true;
    return false;
}
void Node::markSeen(const Packet& p) {
    uint8_t h[8];
    p.packetHash(h);
    std::memcpy(seen_[seenNext_], h, 8);
    seenNext_ = (seenNext_ + 1) % 160;
    seenTotal_++;
}

double Node::frameAir(const Packet& p) const { return airtimeSec(cfg_.radio, p.bytes().size()); }

void Node::queue(const Packet& p, double due, int prio, const std::string& what) {
    Outgoing o;
    o.bytes = p.bytes();
    o.due = due;
    o.priority = prio;
    o.what = what;
    out_.push_back(std::move(o));
}

void Node::addMsg(Message m) {
    if (!m.id) m.id = nextId_++;
    msgs_.push_back(std::move(m));
    while (msgs_.size() > cfg_.maxMessages) msgs_.pop_front();
}

bool Node::popDue(double now, Outgoing& o) {
    int best = -1;
    for (size_t i = 0; i < out_.size(); i++) {
        if (out_[i].due > now) continue;
        if (best < 0 || out_[i].priority < out_[(size_t)best].priority ||
            (out_[i].priority == out_[(size_t)best].priority && out_[i].due < out_[(size_t)best].due)) best = (int)i;
    }
    if (best < 0) return false;
    o = std::move(out_[(size_t)best]);
    out_.erase(out_.begin() + best);
    return true;
}

double Node::nextDue() const {
    double t = -1;
    for (const auto& o : out_) if (t < 0 || o.due < t) t = o.due;
    return t;
}

size_t Node::dropStale(double before) {
    const size_t n0 = out_.size();
    out_.erase(std::remove_if(out_.begin(), out_.end(), [&](const Outgoing& o) {
        const bool own = o.what.rfind("message", 0) == 0 || o.what.rfind("channel", 0) == 0 || o.what.rfind("advert", 0) == 0;
        return !own && o.due < before;
    }), out_.end());
    return n0 - out_.size();
}

void Node::noteSent(const Outgoing& o, double now) {
    duty_.add(now, airtimeSec(cfg_.radio, o.bytes.size()));
    for (auto& t : pend_) if (t.sentAt < 0 && o.what.find("#" + std::to_string(t.id) + " ") != std::string::npos) {
        // the ACK timer starts when the frame has gone out (the firmware starts it at the send; a half-duplex radio waits its turn here)
        const double air = airtimeSec(cfg_.radio, o.bytes.size());
        Contact* c = findContactHex(t.pubHex);
        const int hops = c && c->outPathLen >= 0 && !t.flood ? (c->outPathLen & 63) : 0;
        const double ms = t.flood ? 500 + 16.0 * air * 1000 : 500 + (air * 1000 * 6 + 250) * (hops + 1);
        t.sentAt = now;
        t.deadline = now + air + ms / 1000.0 + cfg_.turnaroundSec;
        if (Message* m = findMsg(t.id)) m->status = Message::Sent;
    }
}

bool Node::sendAdvert(double now, bool flood) {
    if (!id_.valid) return false;
    AdvertData a;
    a.type = AdvChat;
    a.name = cfg_.name;
    a.hasPos = cfg_.sharePosition;
    a.lat = cfg_.lat; a.lon = cfg_.lon;
    Packet p = buildAdvert(id_, uniqueTime(), a);
    if (flood) floodOf(p, cfg_.radio.pathHashSize);
    else directOf(p, 0, {});                                     // sendZeroHop
    markSeen(p);
    queue(p, now, flood ? 3 : 0, flood ? "advert" : "advert (zero hop)");
    return true;
}

bool Node::attemptText(PendingText& t, double now) {
    Contact* c = findContactHex(t.pubHex);
    if (!c) return false;
    uint8_t secret[32];
    sharedSecret(id_, c->pub, secret);
    Packet p;
    uint32_t ack = 0;
    if (!buildText(id_, c->pub, secret, t.ts, t.attempt, t.text, p, ack)) return false;
    t.acks.push_back(ack);
    t.flood = c->outPathLen < 0;
    if (t.flood) floodOf(p, cfg_.radio.pathHashSize);
    else directOf(p, (uint8_t)c->outPathLen, c->outPath);
    markSeen(p);
    t.sentAt = -1;
    t.deadline = 1e300;
    char what[160];
    snprintf(what, sizeof what, "message #%u to %s (attempt %d, %s)", t.id, c->name.c_str(), t.attempt + 1, t.flood ? "flood" : "direct");
    queue(p, now, t.flood ? 1 : 0, what);
    if (Message* m = findMsg(t.id)) { m->attempts = t.attempt + 1; m->status = Message::Queued; }
    return true;
}

bool Node::sendText(const std::string& pubHex, const std::string& text, double now, std::string* why) {
    if (!id_.valid) { if (why) *why = "no identity yet"; return false; }
    Contact* c = findContactHex(pubHex);
    if (!c) { if (why) *why = "not in the contact list (its advert has not been heard)"; return false; }
    if (text.empty()) { if (why) *why = "nothing to send"; return false; }
    PendingText t;
    t.id = nextId_++;
    t.pubHex = c->pubHex();
    t.text = text.substr(0, utf8Prefix(text, kMaxText));
    t.ts = uniqueTime();
    Message m;
    m.kind = Message::Direct; m.peer = t.pubHex; m.text = t.text; m.senderTime = t.ts; m.at = (double)t.ts; m.outgoing = true;
    m.status = Message::Queued; m.id = t.id;
    addMsg(m);
    if (!attemptText(t, now)) { if (Message* mm = findMsg(t.id)) mm->status = Message::Failed; if (why) *why = "the message could not be built"; return false; }
    pend_.push_back(std::move(t));
    return true;
}

bool Node::sendChannel(const std::string& name, const std::string& text, double now, std::string* why) {
    const Channel* ch = nullptr;
    for (const auto& c : channels_) if (c.name == name) ch = &c;
    if (!ch) { if (why) *why = "no such channel"; return false; }
    if (text.empty()) { if (why) *why = "nothing to send"; return false; }
    const uint32_t ts = uniqueTime();
    Packet p = buildGroupText(*ch, ts, cfg_.name, text);
    floodOf(p, cfg_.radio.pathHashSize);
    markSeen(p);
    Message m;
    m.kind = Message::Group; m.peer = ch->name; m.text = text.substr(0, utf8Prefix(text, kMaxText > cfg_.name.size() + 2 ? kMaxText - cfg_.name.size() - 2 : 0));
    m.senderTime = ts; m.at = ts; m.outgoing = true; m.status = Message::Queued;
    m.id = nextId_++;
    addMsg(m);
    queue(p, now, 1, "channel message #" + std::to_string(m.id) + " on " + ch->name);
    return true;
}

void Node::resetPath(const std::string& pubHex) {
    if (Contact* c = findContactHex(pubHex)) { c->outPathLen = -1; c->outPath.clear(); }
}
bool Node::removeContact(const std::string& pubHex) {
    for (size_t i = 0; i < contacts_.size(); i++) if (contacts_[i].pubHex() == pubHex) { contacts_.erase(contacts_.begin() + (long)i); return true; }
    return false;
}
bool Node::addChannel(const Channel& c) {
    for (const auto& e : channels_) if (e.name == c.name) return false;
    channels_.push_back(c);
    return true;
}
bool Node::removeChannel(const std::string& name) {
    if (name == "Public") return false;
    for (size_t i = 0; i < channels_.size(); i++) if (channels_[i].name == name) { channels_.erase(channels_.begin() + (long)i); return true; }
    return false;
}

void Node::tick(double now) {
    for (size_t i = 0; i < pend_.size();) {
        PendingText& t = pend_[i];
        if (t.sentAt >= 0 && now > t.deadline) {
            if (t.attempt + 1 < cfg_.maxAttempts) {
                t.attempt++;
                Contact* c = findContactHex(t.pubHex);
                if (c && t.attempt == cfg_.maxAttempts - 1 && c->outPathLen >= 0) {   // the last try floods: the route may be gone
                    if (log) log("MeshCore: no ACK from " + c->name + " along the learned route; the route is dropped and the last try floods");
                    c->outPathLen = -1; c->outPath.clear();
                }
                if (attemptText(t, now)) { i++; continue; }
            }
            if (Message* m = findMsg(t.id)) m->status = Message::Failed;
            pend_.erase(pend_.begin() + (long)i);
            continue;
        }
        i++;
    }
}

void Node::onAckValue(const uint8_t* ack, double now) {
    const uint32_t a = get32(ack);
    for (size_t i = 0; i < pend_.size(); i++) {
        const PendingText& t = pend_[i];
        if (std::find(t.acks.begin(), t.acks.end(), a) == t.acks.end()) continue;
        if (Message* m = findMsg(t.id)) { m->status = Message::Delivered; m->rtt = t.sentAt >= 0 ? now - t.sentAt : 0; }
        pend_.erase(pend_.begin() + (long)i);
        return;
    }
}

void Node::routeFlood(Packet& p, double now) {
    // Mesh::routeRecvPacket: a repeater appends its hash and sends the flood again after 0..4 half airtimes
    if (!cfg_.repeater || !p.flood() || !id_.valid) return;
    const int n = p.hashCount(), hs = p.hashSize();
    if ((n + 1) * hs > (int)kMaxPath) return;
    for (int k = 0; k < hs; k++) p.path.push_back(id_.pub[k]);
    p.pathLen = (uint8_t)((p.pathLen & 0xC0) | (n + 1));
    uint8_t r = 0;
    random(&r, 1);
    const double t = frameAir(p) * 52.0 / 50.0 / 2.0;
    queue(p, now + (r % 5) * t, p.hashCount(), "repeat (flood)");
    repeated_++;
}

void Node::onAdvert(Packet& p, double now) {
    if (p.payload.size() < 32 + 4 + 64) return;
    const uint8_t* pub = p.payload.data();
    if (std::memcmp(pub, id_.pub, 32) == 0) return;              // our own advert
    if (wasSeen(p)) return;
    markSeen(p);
    const uint32_t ts = get32(p.payload.data() + 32);
    const uint8_t* sig = p.payload.data() + 36;
    size_t adl = p.payload.size() - 100;
    if (adl > kMaxAdvertData) adl = kMaxAdvertData;
    const uint8_t* ad = p.payload.data() + 100;
    std::vector<uint8_t> msg(pub, pub + 32);
    put32(msg, ts);
    msg.insert(msg.end(), ad, ad + adl);
    if (!ed25519Verify(sig, msg.data(), msg.size(), pub)) { if (log) log("MeshCore: an advert with a bad signature was ignored"); return; }
    AdvertData a;
    const bool parsed = parseAdvertData(ad, adl, a);
    Contact* c = findContact(pub);
    if (parsed && !a.name.empty()) {
        if (c && ts <= c->lastAdvert) { routeFlood(p, now); return; }   // replay: the contact is not changed (BaseChatMesh::onAdvertRecv)
        if (!c) {
            if (contacts_.size() >= cfg_.maxContacts) {          // the one heard longest ago leaves
                auto it = std::min_element(contacts_.begin(), contacts_.end(), [](const Contact& x, const Contact& y) { return x.heardAt < y.heardAt; });
                contacts_.erase(it);
            }
            Contact nc;
            std::memcpy(nc.pub, pub, 32);
            contacts_.push_back(nc);
            c = &contacts_.back();
            if (log) log("MeshCore: new contact " + a.name);
        }
        c->name = a.name;
        c->type = a.type;
        if (a.hasPos) { c->hasPos = true; c->lat = a.lat; c->lon = a.lon; }
        c->lastAdvert = ts;
        c->heardAt = unixClock ? unixClock() : (double)std::time(nullptr);
        c->hops = p.flood() ? p.hashCount() : 0;
        c->snrDb = p.snrDb;
    }
    routeFlood(p, now);
}

void Node::onGroup(Packet& p, double now) {
    if (p.payload.size() < 4) return;
    if (wasSeen(p)) return;
    markSeen(p);
    const uint8_t h = p.payload[0];
    for (const auto& ch : channels_) {
        if (ch.hash != h) continue;
        std::vector<uint8_t> d;
        if (!macThenDecrypt(ch.secret, p.payload.data() + 1, p.payload.size() - 1, d)) continue;
        if (p.type() == TGrpTxt && d.size() >= 5 && (d[4] >> 2) == 0) {
            std::string s((const char*)d.data() + 5, d.size() - 5);
            const size_t z = s.find('\0');
            if (z != std::string::npos) s.resize(z);
            Message m;
            m.kind = Message::Group; m.peer = ch.name; m.senderTime = get32(d.data());
            m.at = unixClock ? unixClock() : (double)std::time(nullptr);
            const size_t col = s.find(": ");
            if (col != std::string::npos) { m.from = s.substr(0, col); m.text = s.substr(col + 2); } else m.text = s;
            m.hops = p.flood() ? p.hashCount() : 0;
            m.snrDb = p.snrDb;
            addMsg(m);
        }
        break;
    }
    routeFlood(p, now);
}

void Node::onPeer(Packet& p, double now) {
    if (p.payload.size() < 2 + 2 + 1) return;                  // i + CIPHER_MAC_SIZE >= payload_len: incomplete
    if (wasSeen(p)) return;
    markSeen(p);
    bool forUs = false;
    if (id_.valid && p.payload[0] == selfHash()) {
        const uint8_t src = p.payload[1];
        for (size_t ci = 0; ci < contacts_.size(); ci++) {
            Contact& from = contacts_[ci];
            if (from.pub[0] != src) continue;
            uint8_t secret[32];
            sharedSecret(id_, from.pub, secret);
            std::vector<uint8_t> d;
            if (!macThenDecrypt(secret, p.payload.data() + 2, p.payload.size() - 2, d)) continue;
            forUs = true;
            if (p.type() == TPath) {
                if (d.empty() || !validPathLen(d[0])) break;
                const uint8_t pl = d[0];
                const size_t bl = (size_t)(pl & 63) * (size_t)((pl >> 6) + 1);
                if (1 + bl + 1 > d.size()) break;
                from.outPathLen = pl;
                from.outPath.assign(d.begin() + 1, d.begin() + 1 + (long)bl);
                if (log) log("MeshCore: route to " + from.name + " learned (" + std::to_string(pl & 63) + " hops)");
                const uint8_t extraType = d[1 + bl] & 0x0F;
                if (extraType == TAck && d.size() >= 1 + bl + 1 + 4) onAckValue(d.data() + 1 + bl + 1, now);
                if (p.flood()) {   // Mesh.cpp: send the path we came in on back, direct along the route just learned
                    uint8_t rnd[4];
                    random(rnd, 4);
                    Packet r = buildPathReturn(src, selfHash(), secret, p.pathLen, p.path, 0, {}, rnd);
                    directOf(r, from.outPathLen < 0 ? 0 : (uint8_t)from.outPathLen, from.outPath);
                    markSeen(r);
                    queue(r, now + 0.5, 1, "path to " + from.name);
                }
            } else if (p.type() == TTxtMsg && d.size() > 5) {
                const uint32_t ts = get32(d.data());
                const int flags = d[4] >> 2;
                size_t tl = 0;
                while (5 + tl < d.size() && d[5 + tl] != 0) tl++;
                if (flags == 0 || flags == 2) {
                    const size_t off = flags == 2 ? 9 : 5;
                    if (flags == 2) { tl = 0; while (9 + tl < d.size() && d[9 + tl] != 0) tl++; }
                    if (off > d.size()) break;
                    Message m;
                    m.kind = Message::Direct; m.peer = from.pubHex(); m.from = from.name;
                    m.text.assign((const char*)d.data() + off, tl);
                    m.senderTime = ts;
                    m.at = unixClock ? unixClock() : (double)std::time(nullptr);
                    m.hops = p.flood() ? p.hashCount() : -1;
                    m.snrDb = p.snrDb;
                    // a retry of a message already shown (same timestamp and text) is acknowledged again but not shown twice
                    bool dup = false;
                    for (const auto& e : msgs_) if (!e.outgoing && e.peer == m.peer && e.senderTime == ts && e.text == m.text) dup = true;
                    if (!dup) addMsg(m);
                    from.heardAt = m.at;
                    // the ACK: BaseChatMesh::onPeerDataRecv
                    std::vector<uint8_t> ack(4);
                    {
                        Sha256 h;
                        h.update(d.data(), off + tl);
                        h.update(flags == 2 ? id_.pub : from.pub, 32);
                        uint8_t dg[32];
                        h.final(dg);
                        std::memcpy(ack.data(), dg, 4);
                    }
                    if (flags == 0) {
                        ack.push_back(off + tl + 1 < d.size() ? d[off + tl + 1] : 0);
                        uint8_t r = 0;
                        random(&r, 1);
                        ack.push_back(r);
                    }
                    if (p.flood()) {
                        uint8_t rnd[4] = {};
                        Packet r = buildPathReturn(src, selfHash(), secret, p.pathLen, p.path, TAck, ack, rnd);
                        floodOf(r, cfg_.radio.pathHashSize);
                        markSeen(r);
                        queue(r, now + 0.2, 2, "path + ACK to " + from.name);
                    } else {
                        Packet a = buildAck(ack.data(), ack.size());
                        if (from.outPathLen < 0) { floodOf(a, cfg_.radio.pathHashSize); markSeen(a); queue(a, now + 0.2, 1, "ACK to " + from.name); }
                        else { directOf(a, (uint8_t)from.outPathLen, from.outPath); markSeen(a); queue(a, now + 0.2, 0, "ACK to " + from.name); }
                    }
                }
            }
            break;
        }
    }
    if (!forUs) routeFlood(p, now);
}

void Node::receive(const uint8_t* b, size_t n, float snr, double now) {
    Packet p;
    if (!parsePacket(b, n, p)) return;
    p.snrDb = snr;
    // direct packets with a path still to go: only the next hop takes them (Mesh::onRecvPacket)
    if (p.direct() && p.type() == TTrace) {
        if (!cfg_.repeater || !id_.valid || p.payload.size() < 9 || p.pathLen >= kMaxPath) return;
        const int sz = p.payload[8] & 3;
        const size_t len = p.payload.size() - 9;
        const size_t off = (size_t)p.pathLen << sz;
        if (off >= len) return;                                  // the trace has reached its end (not ours to report)
        if (off + ((size_t)1 << sz) > len) return;
        if (std::memcmp(&p.payload[9 + off], id_.pub, (size_t)1 << sz) != 0 || wasSeen(p)) return;
        markSeen(p);
        p.path.push_back((uint8_t)(int8_t)std::lround(snr * 4));
        p.pathLen++;
        queue(p, now, 5, "repeat (trace)");
        repeated_++;
        return;
    }
    if (p.direct() && p.type() == TControl && !p.payload.empty() && (p.payload[0] & 0x80)) return;
    if (p.direct() && p.hashCount() > 0) {
        if (p.type() == TAck && p.payload.size() >= 4) onAckValue(p.payload.data(), now);
        const int hs = p.hashSize();
        if (cfg_.repeater && id_.valid && (int)p.path.size() >= hs && std::memcmp(p.path.data(), id_.pub, (size_t)hs) == 0 && !wasSeen(p)) {
            markSeen(p);
            p.path.erase(p.path.begin(), p.path.begin() + hs);   // removeSelfFromPath
            p.pathLen = (uint8_t)((p.pathLen & 0xC0) | (p.hashCount() - 1));
            queue(p, now, 0, "repeat (direct)");
            repeated_++;
        }
        return;
    }
    switch (p.type()) {
    case TAck:
        if (p.payload.size() >= 4 && !wasSeen(p)) {
            markSeen(p);
            const size_t before = pend_.size();
            onAckValue(p.payload.data(), now);
            if (pend_.size() == before) routeFlood(p, now);       // an ACK for us is not passed on
        }
        break;
    case TPath: case TReq: case TResponse: case TTxtMsg:
        onPeer(p, now);
        break;
    case TAnonReq:
        if (p.payload.size() > 1 + 32 + 2 && !wasSeen(p)) { markSeen(p); if (p.payload[0] != selfHash() || !id_.valid) routeFlood(p, now); }
        break;
    case TGrpTxt: case TGrpData:
        onGroup(p, now);
        break;
    case TAdvert:
        onAdvert(p, now);
        break;
    case TMultipart:
        if (p.payload.size() >= 5 && (p.payload[0] & 0x0F) == TAck) {
            Packet tmp = p;
            tmp.payload.erase(tmp.payload.begin());
            if (!wasSeen(tmp)) { markSeen(tmp); onAckValue(tmp.payload.data(), now); }
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- save / load

std::string Node::save() const {
    std::ostringstream o;
    o << "meshcore-node 1\n";
    if (id_.valid) o << "identity " << identityPrivateHex(id_) << "\n";
    for (const auto& c : contacts_) {
        o << "contact " << c.pubHex() << " " << c.type << " " << c.lastAdvert << " " << (long long)c.heardAt << " " << (c.hasPos ? 1 : 0) << " ";
        char b[64]; snprintf(b, sizeof b, "%.6f %.6f", c.lat, c.lon); o << b;
        o << " " << c.outPathLen << " " << (c.outPath.empty() ? std::string("-") : hexOf(c.outPath.data(), c.outPath.size())) << " " << c.hops << " " << enc(c.name) << "\n";
    }
    for (const auto& ch : channels_) if (ch.name != "Public") o << "channel " << channelKeyHex(ch) << " " << enc(ch.name) << "\n";
    for (const auto& m : msgs_)
        o << "msg " << m.kind << " " << (m.outgoing ? 1 : 0) << " " << (m.status == Message::Queued || m.status == Message::Sent ? (int)Message::Failed : m.status) << " "
          << m.senderTime << " " << (long long)m.at << " " << m.hops << " " << enc(m.peer) << " " << enc(m.from) << " " << enc(m.text) << "\n";
    return o.str();
}

bool Node::load(const std::string& s) {
    std::istringstream in(s);
    std::string line;
    if (!std::getline(in, line) || line.rfind("meshcore-node 1", 0) != 0) return false;
    Identity id;
    std::vector<Contact> cs;
    std::vector<Channel> chs{publicChannel()};
    std::deque<Message> ms;
    while (std::getline(in, line)) {
        std::istringstream l(line);
        std::string kind;
        l >> kind;
        if (kind == "identity") {
            std::string hx; l >> hx;
            identityFromPrivateHex(hx, id);
        } else if (kind == "contact") {
            Contact c; std::string pub, path, name; long long heard = 0; int hasPos = 0;
            l >> pub >> c.type >> c.lastAdvert >> heard >> hasPos >> c.lat >> c.lon >> c.outPathLen >> path >> c.hops >> name;
            std::vector<uint8_t> raw;
            if (!l || !hexDecode(pub, raw) || raw.size() != 32) continue;
            std::memcpy(c.pub, raw.data(), 32);
            c.heardAt = (double)heard; c.hasPos = hasPos != 0; c.name = dec(name);
            if (path != "-") hexDecode(path, c.outPath);
            if (c.outPathLen >= 0 && !validPathLen((uint8_t)c.outPathLen)) { c.outPathLen = -1; c.outPath.clear(); }
            cs.push_back(c);
        } else if (kind == "channel") {
            std::string key, name; l >> key >> name;
            Channel c;
            if (makeChannel(dec(name), key, c)) chs.push_back(c);
        } else if (kind == "msg") {
            Message m; int out = 0; long long at = 0; std::string peer, from, text;
            l >> m.kind >> out >> m.status >> m.senderTime >> at >> m.hops >> peer >> from >> text;
            if (!l) continue;
            m.outgoing = out != 0; m.at = (double)at; m.peer = dec(peer); m.from = dec(from); m.text = dec(text);
            m.id = nextId_++;
            ms.push_back(m);
        }
    }
    id_ = id;
    contacts_ = cs;
    channels_ = chs;
    msgs_ = ms;
    return true;
}

} // namespace meshcore
} // namespace dect2
