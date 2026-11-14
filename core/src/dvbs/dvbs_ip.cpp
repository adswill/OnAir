// IP over DVB-S/S2 (see dvbs_ip.h). Adapted from DecSAT by Arclamp (CasualArclamp), GPL-3.0-or-later, https://github.com/CasualArclamp/DecSAT
// (crates decsat-gse and decsat-ip), rewritten in C++.
#include "dect2/dvbs/dvbs_ip.h"
#include "dect2/wirecap/wire.h"
#include "dect2/wirecap/wire.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace dect2 {
namespace dvbsip {

const Variant kVariants[4] = {
    {LengthMode::Standard, FragMode::Plain},
    {LengthMode::Standard, FragMode::Split},
    {LengthMode::HeaderIncluded, FragMode::Plain},
    {LengthMode::HeaderIncluded, FragMode::Split},
};

const char* Variant::label() const {
    if (length == LengthMode::Standard) return frag == FragMode::Plain ? "standard" : "split frag id";
    return frag == FragMode::Plain ? "length incl. header" : "length incl. header, split frag id";
}

std::string Label::text() const {
    if (type == 2) return "broadcast";
    char s[24];
    std::string r;
    for (size_t i = 0; i < size(); i++) { snprintf(s, sizeof s, i ? ":%02x" : "%02x", b[i]); r += s; }
    return r;
}

// ---------------------------------------------------------------- IP

uint16_t inetChecksum(const uint8_t* p, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i += 2) sum += (uint32_t)p[i] << 8 | (i + 1 < n ? p[i + 1] : 0);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static bool portsOf(uint8_t proto, const uint8_t* l4, size_t n, uint16_t& s, uint16_t& d) {
    if ((proto != kProtoTcp && proto != kProtoUdp) || n < 4) return false;
    s = (uint16_t)(l4[0] << 8 | l4[1]); d = (uint16_t)(l4[2] << 8 | l4[3]);
    return true;
}

static bool parseV4(const uint8_t* d, size_t n, IpInfo& o) {
    if (n < 20) return false;
    const size_t ihl = (size_t)(d[0] & 0x0F) * 4, len = (size_t)(d[2] << 8 | d[3]);
    if (ihl < 20 || len < ihl || len > n || inetChecksum(d, ihl) != 0) return false;
    o = IpInfo();
    memcpy(o.src.data(), d + 12, 4); memcpy(o.dst.data(), d + 16, 4);
    o.protocol = d[9]; o.len = len;
    const bool first = (((d[6] << 8) | d[7]) & 0x1FFF) == 0;   // ports only in the first fragment
    if (first) o.hasPorts = portsOf(d[9], d + ihl, len - ihl, o.sport, o.dport);
    return true;
}

static bool parseV6(const uint8_t* d, size_t n, IpInfo& o) {
    if (n < 40) return false;
    const size_t len = 40 + (size_t)(d[4] << 8 | d[5]);
    if (len > n) return false;
    o = IpInfo();
    o.v6 = true;
    memcpy(o.src.data(), d + 8, 16); memcpy(o.dst.data(), d + 24, 16);
    o.protocol = d[6]; o.len = len;
    o.hasPorts = portsOf(d[6], d + 40, len - 40, o.sport, o.dport);
    return true;
}

bool parseIp(const uint8_t* d, size_t n, IpInfo& o) {
    if (n == 0) return false;
    switch (d[0] >> 4) { case 4: return parseV4(d, n, o); case 6: return parseV6(d, n, o); default: return false; }
}

int udpChecksumOk(const uint8_t* ip, const IpInfo& info) {
    if (info.protocol != kProtoUdp || info.v6) return -1;
    const size_t ihl = (size_t)(ip[0] & 0x0F) * 4;
    if (((ip[6] << 8 | ip[7]) & 0x3FFF) != 0) return -1;   // more fragments, or an offset: not the whole datagram
    if (info.len < ihl + 8) return -1;
    const uint8_t* udp = ip + ihl;
    const size_t avail = info.len - ihl;
    if (udp[6] == 0 && udp[7] == 0) return -1;
    const size_t ulen = (size_t)(udp[4] << 8 | udp[5]);
    if (ulen < 8 || ulen > avail) return -1;
    std::vector<uint8_t> s;
    s.insert(s.end(), ip + 12, ip + 20);
    s.push_back(0); s.push_back(kProtoUdp);
    s.push_back((uint8_t)(ulen >> 8)); s.push_back((uint8_t)ulen);
    s.insert(s.end(), udp, udp + ulen);
    return inetChecksum(s.data(), s.size()) == 0 ? 1 : 0;
}

std::vector<std::pair<size_t, IpInfo>> blindIpv4Search(const uint8_t* d, size_t n) {
    std::vector<std::pair<size_t, IpInfo>> out;
    size_t i = 0;
    while (i + 20 <= n) {
        IpInfo info;
        if ((d[i] >> 4) == 4 && (d[i] & 0x0F) >= 5 && parseV4(d + i, n - i, info)) { out.push_back({i, info}); i += info.len; continue; }
        i++;
    }
    return out;
}

std::string addrString(const std::array<uint8_t, 16>& a, bool v6) {
    char s[64];
    if (!v6) { snprintf(s, sizeof s, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]); return s; }
    std::string r;
    for (int i = 0; i < 16; i += 2) { snprintf(s, sizeof s, i ? ":%x" : "%x", a[(size_t)i] << 8 | a[(size_t)i + 1]); r += s; }
    return r;
}

const char* protocolName(uint8_t p) {
    switch (p) { case kProtoTcp: return "TCP"; case kProtoUdp: return "UDP"; case kProtoIcmp: return "ICMP"; case kProtoIcmpV6: return "ICMPv6"; case 2: return "IGMP"; case 47: return "GRE"; case 50: return "ESP"; case 51: return "AH"; case 89: return "OSPF"; case 132: return "SCTP"; default: return "other"; }
}

std::vector<uint8_t> buildUdpV4(const uint8_t src[4], const uint8_t dst[4], uint16_t sport, uint16_t dport, const std::vector<uint8_t>& payload, bool withUdpChecksum) {
    const size_t len = 28 + payload.size();
    std::vector<uint8_t> p(len, 0);
    p[0] = 0x45; p[2] = (uint8_t)(len >> 8); p[3] = (uint8_t)len; p[8] = 64; p[9] = kProtoUdp;
    memcpy(&p[12], src, 4); memcpy(&p[16], dst, 4);
    const uint16_t c = inetChecksum(p.data(), 20);
    p[10] = (uint8_t)(c >> 8); p[11] = (uint8_t)c;
    p[20] = (uint8_t)(sport >> 8); p[21] = (uint8_t)sport; p[22] = (uint8_t)(dport >> 8); p[23] = (uint8_t)dport;
    const size_t ulen = 8 + payload.size();
    p[24] = (uint8_t)(ulen >> 8); p[25] = (uint8_t)ulen;
    memcpy(&p[28], payload.data(), payload.size());
    if (withUdpChecksum) {
        std::vector<uint8_t> s;
        s.insert(s.end(), p.begin() + 12, p.begin() + 20);
        s.push_back(0); s.push_back(kProtoUdp); s.push_back((uint8_t)(ulen >> 8)); s.push_back((uint8_t)ulen);
        s.insert(s.end(), p.begin() + 20, p.end());
        uint16_t u = inetChecksum(s.data(), s.size());
        if (u == 0) u = 0xFFFF;   // a computed zero is sent as all ones (RFC 768)
        p[26] = (uint8_t)(u >> 8); p[27] = (uint8_t)u;
    }
    return p;
}

std::vector<std::vector<uint8_t>> fragmentIpv4(const std::vector<uint8_t>& d, size_t mtu) {
    std::vector<std::vector<uint8_t>> out;
    const size_t body = d.size() - 20, step = (mtu - 20) / 8 * 8;
    for (size_t k = 0; k * step < body; k++) {
        const size_t off = k * step, take = std::min(step, body - off);
        std::vector<uint8_t> h(d.begin(), d.begin() + 20);
        const bool more = off + take < body;
        const uint16_t fo = (uint16_t)((off / 8) | (more ? 0x2000 : 0));
        h[2] = (uint8_t)((20 + take) >> 8); h[3] = (uint8_t)(20 + take);
        h[6] = (uint8_t)(fo >> 8); h[7] = (uint8_t)fo; h[10] = h[11] = 0;
        const uint16_t c = inetChecksum(h.data(), 20);
        h[10] = (uint8_t)(c >> 8); h[11] = (uint8_t)c;
        h.insert(h.end(), d.begin() + 20 + (long)off, d.begin() + 20 + (long)off + (long)take);
        out.push_back(std::move(h));
    }
    return out;
}

uint32_t crc32Msb(const uint8_t* p, size_t n, uint32_t init) {
    // built once, thread-safely (a plain flag raced when two receiver threads made their first CRC together)
    static const std::array<uint32_t, 256> t = [] {
        std::array<uint32_t, 256> a{};
        for (uint32_t v = 0; v < 256; v++) { uint32_t c = v << 24; for (int i = 0; i < 8; i++) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1; a[v] = c; }
        return a;
    }();
    uint32_t c = init;
    for (size_t i = 0; i < n; i++) c = (c << 8) ^ t[((c >> 24) ^ p[i]) & 255];
    return c;
}

// ---------------------------------------------------------------- fragments

static const uint64_t kMaxAge = 4096;
static const size_t kMaxPending = 64;

bool Reassembler::complete(Pending& p, std::vector<uint8_t>& payload) {
    if (p.end < 0 || p.header.empty()) return false;
    std::sort(p.pieces.begin(), p.pieces.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    size_t have = 0;
    for (const auto& pc : p.pieces) { if (pc.first > have) return false; have = std::max(have, pc.first + pc.second.size()); }
    if ((long)have < p.end) return false;
    payload.assign((size_t)p.end, 0);
    for (const auto& pc : p.pieces) {
        const size_t n = std::min(pc.second.size(), (size_t)p.end > pc.first ? (size_t)p.end - pc.first : 0);   // a piece past the end is cut short
        memcpy(&payload[pc.first], pc.second.data(), n);
    }
    return true;
}

bool Reassembler::push(const uint8_t* ip, const IpInfo& info, std::vector<uint8_t>& out) {
    packets_++;
    if ((ip[0] >> 4) != 4 || info.len < 20) { out.assign(ip, ip + info.len); return true; }
    const uint16_t fo = (uint16_t)(ip[6] << 8 | ip[7]);
    const bool more = (fo & 0x2000) != 0;
    const size_t offset = (size_t)(fo & 0x1FFF) * 8;
    if (!more && offset == 0) { out.assign(ip, ip + info.len); return true; }
    fragments++;
    const size_t ihl = (size_t)(ip[0] & 0x0F) * 4;
    if (info.len < ihl) return false;
    const uint8_t* data = ip + ihl;
    const size_t dn = info.len - ihl;
    uint32_t s, d;
    memcpy(&s, ip + 12, 4); memcpy(&d, ip + 16, 4);
    const uint16_t id = (uint16_t)(ip[4] << 8 | ip[5]);
    // expire the old
    const size_t before = pending_.size();
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return packets_ - p.touched > kMaxAge; }), pending_.end());
    dropped += before - pending_.size();
    size_t i = pending_.size();
    for (size_t k = 0; k < pending_.size(); k++) if (pending_[k].src == s && pending_[k].dst == d && pending_[k].proto == ip[9] && pending_[k].id == id) { i = k; break; }
    if (i == pending_.size()) {
        if (pending_.size() >= kMaxPending) {
            size_t oldest = 0;
            for (size_t k = 1; k < pending_.size(); k++) if (pending_[k].touched < pending_[oldest].touched) oldest = k;
            pending_.erase(pending_.begin() + (long)oldest); dropped++;
        }
        Pending p; p.src = s; p.dst = d; p.proto = ip[9]; p.id = id; p.touched = packets_;
        pending_.push_back(std::move(p));
        i = pending_.size() - 1;
    }
    Pending& p = pending_[i];
    p.touched = packets_;
    if (offset + dn > 65535) return false;
    if (offset == 0) p.header.assign(ip, ip + ihl);
    if (!more) p.end = (long)(offset + dn);
    p.pieces.push_back({offset, std::vector<uint8_t>(data, data + dn)});
    std::vector<uint8_t> payload;
    if (!complete(p, payload)) return false;
    std::vector<uint8_t> h = std::move(p.header);
    pending_.erase(pending_.begin() + (long)i);
    const size_t total = h.size() + payload.size();
    if (total > 65535) { dropped++; return false; }
    h[2] = (uint8_t)(total >> 8); h[3] = (uint8_t)total; h[6] = h[7] = 0; h[10] = h[11] = 0;
    const uint16_t c = inetChecksum(h.data(), h.size());
    h[10] = (uint8_t)(c >> 8); h[11] = (uint8_t)c;
    h.insert(h.end(), payload.begin(), payload.end());
    reassembled++;
    out = std::move(h);
    return true;
}

// ---------------------------------------------------------------- GSE decoder

static const size_t kMaxPdu = 65535;
static size_t labelLen(int lt) { return lt == 0 ? 6 : (lt == 1 ? 3 : 0); }

static bool readLabel(const uint8_t* b, size_t n, int lt, const Label& last, Label& out, size_t& used) {
    const size_t k = labelLen(lt);
    if (n < k) return false;
    if (lt == 0 || lt == 1) { out.type = lt; out.b.fill(0); memcpy(out.b.data(), b, k); used = k; }
    else if (lt == 2) { out = Label(); used = 0; }
    else { out = last; used = 0; }   // type 3: the previous packet's label in this field
    return true;
}

void GseDecoder::dataField(const uint8_t* field, size_t n, bool verified, std::vector<Pdu>& out) {
    verified_ = verified;
    size_t i = 0;
    Label last;
    while (i + 2 <= n) {
        const unsigned h = (unsigned)(field[i] << 8 | field[i + 1]);
        const bool s = (h >> 15) & 1, e = (h >> 14) & 1;
        const int lt = (int)((h >> 12) & 3);
        const size_t gseLength = h & 0x0FFF;
        if (!s && !e && lt == 0 && gseLength == 0) { stats.padding++; break; }
        size_t bodyLen;
        if (variant.length == LengthMode::Standard) bodyLen = gseLength;
        else { if (gseLength < 2) { stats.errors++; break; } bodyLen = gseLength - 2; }
        const size_t bs = i + 2;
        if (bs + bodyLen > n) { stats.errors++; break; }
        const uint8_t* body = field + bs;
        i = bs + bodyLen;
        stats.packets++;
        bool ok;
        if (s && e) ok = whole(body, bodyLen, lt, last, out);
        else if (s) ok = start(body, bodyLen, lt, last);
        else if (!e) ok = middle(body, bodyLen);
        else ok = end(body, bodyLen, out);
        if (!ok) { stats.errors++; break; }
    }
}

bool GseDecoder::whole(const uint8_t* b, size_t n, int lt, Label& last, std::vector<Pdu>& out) {
    if (n < 2) return false;
    Pdu p;
    p.protocol = (uint16_t)(b[0] << 8 | b[1]);
    size_t used;
    if (!readLabel(b + 2, n - 2, lt, last, p.label, used)) return false;
    last = p.label;
    stats.whole++;
    p.data.assign(b + 2 + used, b + n);
    out.push_back(std::move(p));
    return true;
}

bool GseDecoder::start(const uint8_t* b, size_t n, int lt, Label& last) {
    if (n < 5) return false;
    Partial p;
    const uint8_t key = fragKey(b[0]);
    p.totalLength = (size_t)(b[1] << 8 | b[2]);
    p.protocol = (uint16_t)(b[3] << 8 | b[4]);
    size_t used;
    if (!readLabel(b + 5, n - 5, lt, last, p.label, used)) return false;
    last = p.label;
    stats.start++;
    p.data.assign(b + 5 + used, b + n);
    p.crcInput.assign(b + 1, b + n);
    p.verified = verified_;
    p.counter = b[0] & 3;
    if (partial_.count(key)) stats.collisions++;
    partial_[key] = std::move(p);
    return true;
}

bool GseDecoder::middle(const uint8_t* b, size_t n) {
    if (n == 0) return false;
    stats.middle++;
    const uint8_t key = fragKey(b[0]);
    auto it = partial_.find(key);
    if (it == partial_.end()) { stats.orphans++; return true; }
    Partial& p = it->second;
    if (p.data.size() + n <= kMaxPdu) {
        p.data.insert(p.data.end(), b + 1, b + n);
        p.crcInput.insert(p.crcInput.end(), b + 1, b + n);
        p.fragments++;
        p.follow(b[0], verified_, variant.frag == FragMode::Split);
    } else partial_.erase(it);
    return true;
}

bool GseDecoder::end(const uint8_t* b, size_t n, std::vector<Pdu>& out) {
    if (n < 5) return false;
    stats.end++;
    const uint8_t key = fragKey(b[0]);
    auto it = partial_.find(key);
    if (it == partial_.end()) { stats.orphans++; return true; }
    Partial p = std::move(it->second);
    partial_.erase(it);
    p.follow(b[0], verified_, variant.frag == FragMode::Split);
    p.data.insert(p.data.end(), b + 1, b + n - 4);
    p.crcInput.insert(p.crcInput.end(), b + 1, b + n - 4);
    const uint32_t sent = (uint32_t)b[n - 4] << 24 | (uint32_t)b[n - 3] << 16 | (uint32_t)b[n - 2] << 8 | b[n - 1];
    const bool crcOk = crc32Mpeg2(p.crcInput.data(), p.crcInput.size()) == sent;
    const bool lengthOk = p.totalLength + 2 == p.crcInput.size();   // the total length covers everything after its own field
    if (crcOk) stats.crcOk++;
    else if (p.verified) stats.vouched++;   // seen live: a vendor whose total length reads 9 short and whose CRC-32 is none of the usual, every BBFRAME carrying a CRC of its own
    else stats.crcBad++;
    if (!crcOk && !lengthOk && !p.verified) return true;
    stats.reassembled++;
    Pdu d;
    d.protocol = p.protocol; d.label = p.label; d.data = std::move(p.data); d.fragments = p.fragments + 1;
    d.crcOk = (crcOk || !p.verified) ? (crcOk ? 1 : 0) : -1;
    out.push_back(std::move(d));
    return true;
}

// ---------------------------------------------------------------- GseIp

static const double kDecay = 0.98;
static const uint64_t kBlindAfter = 50;
static const size_t kMaxLabels = 4096, kMaxAddrs = 4, kMaxKept = 4096;
static const uint64_t kEntropyBytes = 32 * 1024;
static const size_t kBbHeader = 10;

GseIp::GseIp() {
    for (const Variant& v : kVariants) dec_.emplace_back(v);
    pdus_.resize(4); scores_.assign(4, 0.0); ipCounts_.assign(4, 0);
}

bool GseIp::frameTrailer(const uint8_t* bb, size_t n) {
    if (n < kBbHeader + 4) return false;
    const uint32_t c = crc32Msb(bb, n - 4, 0);
    return bb[n - 4] == (uint8_t)(c >> 24) && bb[n - 3] == (uint8_t)(c >> 16) && bb[n - 2] == (uint8_t)(c >> 8) && bb[n - 1] == (uint8_t)c;
}

bool GseIp::ipOf(const Pdu& p, const uint8_t*& ip, IpInfo& info) {
    const uint8_t* d = p.data.data();
    size_t n = p.data.size();
    if (p.protocol == 0x0800 || p.protocol == 0x86DD) {}
    else if (p.protocol == 0x0001 && n > 14) {   // ULE/GSE bridged frame (RFC 4326): the PDU is an Ethernet frame
        const unsigned et = (unsigned)(d[12] << 8 | d[13]);
        if (et != 0x0800 && et != 0x86DD) return false;
        d += 14; n -= 14;
    } else return false;
    if (!parseIp(d, n, info)) return false;
    ip = d;
    return true;
}

void GseIp::bbframe(const uint8_t* bb, size_t n, std::vector<IpPacket>& out) {
    if (n < kBbHeader) return;
    if (frameTrailer(bb, n)) { trailers_++; field_(bb + kBbHeader, n - kBbHeader - 4, true, out); }
    else field_(bb + kBbHeader, n - kBbHeader, false, out);
}

int GseIp::chosen() const {
    if (forced >= 0) return forced;
    int k = 0;
    for (int i = 1; i < 4; i++) if (scores_[(size_t)i] > scores_[(size_t)k]) k = i;   // the standard on a tie
    return scores_[(size_t)k] > 0.0 ? k : -1;
}

int GseIp::bestFit() const {
    int best = 0;
    uint64_t bm = ~0ull;
    for (int k = 0; k < 4; k++) { const GseStats& s = dec_[(size_t)k].stats; const uint64_t m = s.errors + s.orphans + s.collisions; if (m < bm) { bm = m; best = k; } }
    return best;
}

std::vector<VariantReport> GseIp::reports() const {
    std::vector<VariantReport> r;
    for (size_t k = 0; k < 4; k++) r.push_back({dec_[k].variant, dec_[k].stats, ipCounts_[k]});
    return r;
}

double GseIp::otherEntropy() const {
    uint64_t total = 0;
    for (uint64_t v : otherBytes_) total += v;
    if (total < kEntropyBytes) return -1;
    double h = 0;
    for (uint64_t v : otherBytes_) if (v) { const double p = (double)v / (double)total; h -= p * std::log2(p); }
    return h;
}

void GseIp::countLabel(const Pdu& p) {
    if (labels_.size() >= kMaxLabels && !labels_.count(p.label)) return;
    LabelStats& e = labels_[p.label];
    e.pdus++; e.bytes += p.data.size(); e.modcods[modcod_] += p.data.size();
    const uint8_t* ip; IpInfo info;
    if (ipOf(p, ip, info)) {
        e.ip++;
        if (e.addrs.size() < kMaxAddrs) {
            bool have = false;
            for (size_t i = 0; i < e.addrs.size(); i++) if (e.addrs[i] == info.dst && e.addrV6[i] == info.v6) have = true;
            if (!have) { e.addrs.push_back(info.dst); e.addrV6.push_back(info.v6); }
        }
    }
}

void GseIp::other(const Pdu& p) {
    otherProtocols_[p.protocol]++;
    for (uint8_t b : p.data) otherBytes_[b]++;
    if (keepOthers_) { if (others_.size() >= kMaxKept) others_.erase(others_.begin()); others_.push_back(p); }
}

void GseIp::field_(const uint8_t* field, size_t n, bool verified, std::vector<IpPacket>& out) {
    fields_++;
    bool anyIp = false;
    for (size_t k = 0; k < 4; k++) {
        pdus_[k].clear();
        dec_[k].dataField(field, n, verified, pdus_[k]);
        size_t valid = 0;
        for (const Pdu& p : pdus_[k]) { const uint8_t* ip; IpInfo i; if (ipOf(p, ip, i)) valid++; }
        scores_[k] = scores_[k] * kDecay + (double)valid;
        ipCounts_[k] += valid;
        anyIp = anyIp || valid > 0;
    }
    fieldsSinceIp_ = anyIp ? 0 : fieldsSinceIp_ + 1;
    const int ch = chosen();
    const size_t k = (size_t)(ch >= 0 ? ch : bestFit());
    for (const Pdu& p : pdus_[k]) countLabel(p);
    if (ch >= 0) {
        for (const Pdu& p : pdus_[k]) {
            const uint8_t* ip; IpInfo info;
            if (ipOf(p, ip, info)) out.push_back({std::vector<uint8_t>(ip, ip + info.len), info});
            else other(p);
        }
    } else for (const Pdu& p : pdus_[k]) other(p);   // no IP from any variant yet: say what the GSE carries instead
    if (ch < 0 && fieldsSinceIp_ >= kBlindAfter) {
        for (const auto& f : blindIpv4Search(field, n)) { blindPackets_++; out.push_back({std::vector<uint8_t>(field + f.first, field + f.first + f.second.len), f.second}); }
    }
}

// ---------------------------------------------------------------- encapsulator

std::vector<uint8_t> GseEncapsulator::header(bool s, bool e, int lt, size_t bodyLen) const {
    const unsigned len = (unsigned)variant.gseLength(bodyLen);
    const unsigned h = (s ? 0x8000u : 0) | (e ? 0x4000u : 0) | ((unsigned)lt << 12) | len;
    return {(uint8_t)(h >> 8), (uint8_t)h};
}

std::vector<uint8_t> GseEncapsulator::fill(size_t len) {
    std::vector<uint8_t> o = packets(len);
    o.resize(len, 0);   // zeros read as the padding header
    return o;
}

std::vector<uint8_t> GseEncapsulator::packets(size_t len) {
    std::vector<uint8_t> out;
    const size_t mb = maxBody();
    auto put = [&](const std::vector<uint8_t>& v) { out.insert(out.end(), v.begin(), v.end()); };
    while (!queue_.empty()) {
        Pending& p = queue_.front();
        const size_t room = len - out.size();
        const size_t lab = p.label.size();
        if (!p.frag) {
            const size_t wholeBody = 2 + lab + p.pdu.size();
            if (2 + wholeBody <= room && wholeBody <= mb) {
                put(header(true, true, p.label.type, wholeBody));
                out.push_back((uint8_t)(p.protocol >> 8)); out.push_back((uint8_t)p.protocol);
                out.insert(out.end(), p.label.b.begin(), p.label.b.begin() + (long)lab);
                out.insert(out.end(), p.pdu.begin(), p.pdu.end());
                queue_.pop_front();
                continue;
            }
            // a start fragment, with at least one PDU byte, and leaving at least one for the end
            const size_t fixed = 2 + 1 + 2 + 2 + lab;
            if (room < fixed + 1 || p.pdu.size() < 2) break;
            const size_t take = std::min({room - fixed, mb - (fixed - 2), p.pdu.size() - 1});
            const uint8_t id = nextId_;
            nextId_ = (uint8_t)((nextId_ + 1) % 64);
            const size_t total = 2 + lab + p.pdu.size();
            std::vector<uint8_t> crcIn{(uint8_t)(total >> 8), (uint8_t)total, (uint8_t)(p.protocol >> 8), (uint8_t)p.protocol};
            crcIn.insert(crcIn.end(), p.label.b.begin(), p.label.b.begin() + (long)lab);
            crcIn.insert(crcIn.end(), p.pdu.begin(), p.pdu.end());
            p.crc = crc32Mpeg2(crcIn.data(), crcIn.size());
            put(header(true, false, p.label.type, 1 + 2 + 2 + lab + take));
            out.push_back(fragByte(id, 0));
            out.push_back((uint8_t)(total >> 8)); out.push_back((uint8_t)total);
            out.push_back((uint8_t)(p.protocol >> 8)); out.push_back((uint8_t)p.protocol);
            out.insert(out.end(), p.label.b.begin(), p.label.b.begin() + (long)lab);
            out.insert(out.end(), p.pdu.begin(), p.pdu.begin() + (long)take);
            p.sent = take; p.frag = true; p.id = id; p.counter = 1;
        } else {
            const size_t rest = p.pdu.size() - p.sent;
            const size_t endBody = 1 + rest + 4;
            if (2 + endBody <= room && endBody <= mb) {
                put(header(false, true, 0, endBody));
                out.push_back(fragByte(p.id, p.counter));
                out.insert(out.end(), p.pdu.begin() + (long)p.sent, p.pdu.end());
                for (int sh = 24; sh >= 0; sh -= 8) out.push_back((uint8_t)(p.crc >> sh));
                queue_.pop_front();
                continue;
            }
            // a middle fragment, leaving at least one byte for the end
            if (room < 4 || rest < 2) break;
            const size_t take = std::min({room - 3, mb - 1, rest - 1});
            put(header(false, false, 0, 1 + take));
            out.push_back(fragByte(p.id, p.counter));
            out.insert(out.end(), p.pdu.begin() + (long)p.sent, p.pdu.begin() + (long)(p.sent + take));
            p.sent += take; p.counter = (uint8_t)(p.counter + 1);
        }
    }
    return out;
}



// ---------------------------------------------------------------- GSE-HEM

static size_t hemPacketLen(const uint8_t* h) { return 2 + (size_t)(((h[0] << 8) | h[1]) & 0x0FFF); }
static bool hemPadding(const uint8_t* h) { return (((h[0] << 8) | h[1]) & 0xF000) == 0 && hemPacketLen(h) == 2; }

void HemDeframer::resyncAt(long start, size_t& p) {
    stats.mismatches++;
    partial_.clear();
    if (start >= 0) { p = (size_t)start; stats.resyncs++; } else synced_ = false;
}

void HemDeframer::dataField(const uint8_t* field, size_t n, uint16_t syncd, std::vector<uint8_t>& out) {
    stats.fields++;
    long start = -1;
    if (syncd != 0xFFFF && syncd % 8 == 0 && (size_t)(syncd / 8) <= n) start = syncd / 8;
    size_t p = 0;
    if (!synced_) {
        if (start >= 0) { p = (size_t)start; synced_ = true; stats.resyncs++; }
        else { stats.unsynced++; return; }
    } else if (!partial_.empty()) {
        // finish the packet left over: its header first, then the body the header announces
        while (partial_.size() < 2 && p < n) partial_.push_back(field[p++]);
        if (partial_.size() >= 2) {
            const size_t need = hemPacketLen(partial_.data());
            const size_t take = std::min(need - partial_.size(), n - p);
            partial_.insert(partial_.end(), field + p, field + p + take);
            p += take;
            if (partial_.size() == need) {
                if (start >= 0 && (size_t)start != p) resyncAt(start, p);   // the next packet does not start where SYNCD says
                else { out.insert(out.end(), partial_.begin(), partial_.end()); stats.packets++; stats.joined++; partial_.clear(); }
            }
        }
        if (!partial_.empty()) {
            // the whole field went into the leftover packet: none can have started in it
            if (start >= 0) resyncAt(start, p); else return;
        }
    } else if (start > 0) {
        resyncAt(start, p);   // a packet should start right at the field's beginning
    }
    // whole packets from p; one running past the end is kept
    while (p < n) {
        if (n - p < 2) { partial_.assign(field + p, field + n); break; }
        if (hemPadding(field + p)) break;
        const size_t len = hemPacketLen(field + p);
        if (p + len <= n) { out.insert(out.end(), field + p, field + p + len); stats.packets++; p += len; }
        else { partial_.assign(field + p, field + n); break; }
    }
}

// ---------------------------------------------------------------- sections, MPE, ULE

static const size_t kMaxSection = 4096 + 3;
static size_t sectionLength(const uint8_t* s) { return (size_t)((s[1] & 0x0F) << 8 | s[2]); }

void SectionAssembler::feed(uint16_t pid, bool pusi, const uint8_t* payload, size_t n, std::vector<std::vector<uint8_t>>& out) {
    auto whole = [](const std::vector<uint8_t>& s) { return s.size() >= 3 && 3 + sectionLength(s.data()) <= kMaxSection && s.size() >= 3 + sectionLength(s.data()); };
    if (pusi) {
        if (n == 0) return;
        const size_t ptr = payload[0];
        if (1 + ptr > n) { partial_.erase(pid); return; }
        auto it = partial_.find(pid);
        if (it != partial_.end()) {
            std::vector<uint8_t> s = std::move(it->second);
            partial_.erase(it);
            s.insert(s.end(), payload + 1, payload + 1 + ptr);
            if (whole(s)) out.emplace_back(s.begin(), s.begin() + (long)(3 + sectionLength(s.data())));
        }
        const uint8_t* rest = payload + 1 + ptr;
        size_t rn = n - 1 - ptr;
        while (rn > 0 && rest[0] != 0xFF) {
            if (rn < 3) { partial_[pid].assign(rest, rest + rn); return; }
            const size_t len = 3 + sectionLength(rest);
            if (rn >= len) { out.emplace_back(rest, rest + len); rest += len; rn -= len; }
            else { partial_[pid].assign(rest, rest + rn); return; }
        }
    } else {
        auto it = partial_.find(pid);
        if (it == partial_.end()) return;
        std::vector<uint8_t> s = std::move(it->second);
        partial_.erase(it);
        s.insert(s.end(), payload, payload + n);
        if (whole(s)) out.emplace_back(s.begin(), s.begin() + (long)(3 + sectionLength(s.data())));
        else if (s.size() < kMaxSection) partial_[pid] = std::move(s);
    }
}

void MpeExtractor::packet(const uint8_t* p, std::vector<std::vector<uint8_t>>& out) {
    const uint16_t pid = (uint16_t)((p[1] & 0x1F) << 8 | p[2]);
    if ((p[1] & 0x80) || pid < 0x20 || pid == 0x1FFF || notMpe_.count(pid)) return;   // errored, SI or null
    const bool pusi = (p[1] & 0x40) != 0;
    const int afc = (p[3] >> 4) & 3;
    if ((p[3] >> 6) != 0 || !(afc & 1)) return;                                         // scrambled, or no payload: never decoded
    size_t at = 4 + ((afc & 2) ? 1 + (size_t)p[4] : 0);
    if (at >= 188) return;
    const uint8_t* payload = p + at;
    const size_t n = 188 - at;
    if (!stats.pids.count(pid)) {
        if (!pusi) return;
        if (n >= 3 && payload[0] == 0 && payload[1] == 0 && payload[2] == 1) { notMpe_[pid] = true; return; }   // a PES start rules a PID out
        const size_t ptr = payload[0];
        if (1 + ptr >= n || payload[1 + ptr] != 0x3E) return;
        stats.pids[pid] = 0;
    }
    std::vector<std::vector<uint8_t>> done;
    sections_.feed(pid, pusi, payload, n, done);
    for (const auto& s : done) section(pid, s, out);
}

void MpeExtractor::section(uint16_t pid, const std::vector<uint8_t>& s, std::vector<std::vector<uint8_t>>& out) {
    if (s.size() < 16 || s[0] != 0x3E) return;
    const bool syntax = (s[1] & 0x80) != 0;
    if (syntax && crc32Mpeg2(s.data(), s.size()) != 0) { stats.badSections++; return; }
    const bool payloadScrambled = ((s[5] >> 4) & 3) != 0, llcSnap = (s[5] & 0x02) != 0;
    if (payloadScrambled || s[6] != 0 || s[7] != 0) { stats.skipped++; return; }
    const uint8_t* d = s.data() + 12;
    size_t dn = s.size() - 16;
    if (llcSnap) {   // AA AA 03, OUI, then the EtherType
        if (dn < 8 || d[0] != 0xAA || d[1] != 0xAA || d[2] != 3) return;
        const unsigned et = (unsigned)(d[6] << 8 | d[7]);
        if (et != 0x0800 && et != 0x86DD) return;
        d += 8; dn -= 8;
    }
    IpInfo info;
    if (parseIp(d, dn, info)) { stats.datagrams++; stats.pids[pid]++; out.emplace_back(d, d + info.len); }
}

std::vector<uint8_t> buildMpeSection(const uint8_t mac[6], const std::vector<uint8_t>& dg) {
    const size_t len = 9 + dg.size() + 4;
    std::vector<uint8_t> s = {0x3E, (uint8_t)(0xB0 | ((len >> 8) & 0x0F)), (uint8_t)len, mac[5], mac[4], 0xC1, 0, 0, mac[3], mac[2], mac[1], mac[0]};
    s.insert(s.end(), dg.begin(), dg.end());
    const uint32_t c = crc32Mpeg2(s.data(), s.size());
    for (int sh = 24; sh >= 0; sh -= 8) s.push_back((uint8_t)(c >> sh));
    return s;
}

std::vector<std::vector<uint8_t>> packetizeSection(uint16_t pid, uint8_t& cc, const std::vector<uint8_t>& sec) {
    std::vector<std::vector<uint8_t>> out;
    std::vector<uint8_t> data{0};
    data.insert(data.end(), sec.begin(), sec.end());
    for (size_t k = 0, off = 0; off < data.size(); k++, off += 184) {
        std::vector<uint8_t> p(188, 0xFF);
        const size_t take = std::min<size_t>(184, data.size() - off);
        p[0] = 0x47; p[1] = (uint8_t)((k == 0 ? 0x40 : 0) | ((pid >> 8) & 0x1F)); p[2] = (uint8_t)pid; p[3] = (uint8_t)(0x10 | (cc & 0x0F));
        cc = (uint8_t)((cc + 1) & 0x0F);
        memcpy(&p[4], &data[off], take);
        out.push_back(std::move(p));
    }
    return out;
}

// ULE: SNDU = D(1) length(15) type(16) [NPA 6] PDU CRC-32; length counts everything after the first two bytes (type, NPA, PDU, CRC)
void UleExtractor::parse(uint16_t pid, Pid& st, bool pusi, const uint8_t* payload, size_t n, std::vector<std::vector<uint8_t>>& out) {
    // take whole SNDUs off the front of the buffer
    auto extract = [&]() {
        size_t at = 0;
        while (st.buf.size() - at >= 2) {
            const uint8_t* b = st.buf.data() + at;
            if (b[0] == 0xFF && b[1] == 0xFF) { st.buf.clear(); return; }   // end indicator: the rest of the payload is padding
            const bool d = (b[0] & 0x80) != 0;
            const size_t len = (size_t)((b[0] & 0x7F) << 8 | b[1]);
            if (len < 6 || len > 32767) { st.buf.clear(); st.synced = false; st.miss++; return; }
            if (st.buf.size() - at < 2 + len) break;   // not complete yet
            if (!st.accepted) st.tries++;
            const uint16_t type = (uint16_t)(b[2] << 8 | b[3]);
            const size_t hdr = 4 + (d ? 0 : 6);
            stats.sndus++;
            if (crc32Mpeg2(b, 2 + len) != 0) { stats.badCrc++; st.buf.clear(); st.synced = false; return; }
            if (2 + len >= hdr + 4) {
                const uint8_t* pdu = b + hdr;
                const size_t pn = 2 + len - hdr - 4;
                IpInfo info;
                bool isIp = false;
                if (type == 0x0800 || type == 0x86DD) isIp = parseIp(pdu, pn, info);
                else if (type == 0x0001 && pn > 14) { const unsigned et = (unsigned)(pdu[12] << 8 | pdu[13]); if ((et == 0x0800 || et == 0x86DD) && parseIp(pdu + 14, pn - 14, info)) { isIp = true; pdu += 14; } }
                if (isIp) {
                    st.good++;
                    if (!st.accepted && st.good >= 3) { st.accepted = true; stats.pids[pid] = 0; }
                    if (st.accepted) { stats.pids[pid]++; stats.ipDatagrams++; out.emplace_back(pdu, pdu + info.len); }
                } else stats.other++;
            }
            at += 2 + len;
        }
        st.buf.erase(st.buf.begin(), st.buf.begin() + (long)at);
        if (st.buf.size() > 40000) { st.buf.clear(); st.synced = false; }
    };
    if (pusi) {
        if (n == 0) return;
        const size_t pp = payload[0];
        if (1 + pp > n) { st.buf.clear(); st.synced = false; return; }
        if (st.synced && pp > 0) { st.buf.insert(st.buf.end(), payload + 1, payload + 1 + pp); extract(); }   // the end of the SNDU in progress
        st.buf.assign(payload + 1 + pp, payload + n);
        st.synced = true;
        extract();
    } else {
        if (!st.synced) return;
        st.buf.insert(st.buf.end(), payload, payload + n);
        extract();
    }
}

void UleExtractor::packet(const uint8_t* p, std::vector<std::vector<uint8_t>>& out) {
    const uint16_t pid = (uint16_t)((p[1] & 0x1F) << 8 | p[2]);
    if ((p[1] & 0x80) || pid < 0x20 || pid == 0x1FFF) return;
    const int afc = (p[3] >> 4) & 3;
    if ((p[3] >> 6) != 0 || !(afc & 1)) return;
    size_t at = 4 + ((afc & 2) ? 1 + (size_t)p[4] : 0);
    if (at >= 188) return;
    if (pids_.size() > 64 && !pids_.count(pid)) return;
    Pid& st = pids_[pid];
    if (st.blocked) return;
    const bool pusi = (p[1] & 0x40) != 0;
    if (!st.accepted && pusi && 188 - at >= 4 && p[at + 1] == 0 && p[at + 2] == 0 && p[at + 3] == 1) { st.blocked = true; return; }   // a PES start: video or audio, not ULE
    if (!st.accepted && st.miss + (int)st.tries > 40) { st.blocked = true; return; }
    if (st.cc != 255 && ((p[3] & 0x0F) != ((st.cc + 1) & 0x0F)) && (p[3] & 0x0F) != st.cc) { st.buf.clear(); st.synced = false; }   // a lost packet: wait for the next payload start
    st.cc = p[3] & 0x0F;
    parse(pid, st, pusi, p + at, 188 - at, out);
}

std::vector<uint8_t> buildUleSndu(uint16_t type, const std::vector<uint8_t>& pdu, const uint8_t* npa) {
    const bool d = npa == nullptr;
    const size_t len = 2 + (d ? 0 : 6) + pdu.size() + 4;
    std::vector<uint8_t> s = {(uint8_t)((d ? 0x80 : 0) | ((len >> 8) & 0x7F)), (uint8_t)len, (uint8_t)(type >> 8), (uint8_t)type};
    if (!d) s.insert(s.end(), npa, npa + 6);
    s.insert(s.end(), pdu.begin(), pdu.end());
    const uint32_t c = crc32Mpeg2(s.data(), s.size());
    for (int sh = 24; sh >= 0; sh -= 8) s.push_back((uint8_t)(c >> sh));
    return s;
}

std::vector<std::vector<uint8_t>> packetizeUle(uint16_t pid, uint8_t& cc, const std::vector<std::vector<uint8_t>>& sndus) {
    std::vector<uint8_t> stream;
    std::vector<size_t> starts;
    for (const auto& s : sndus) { starts.push_back(stream.size()); stream.insert(stream.end(), s.begin(), s.end()); }
    std::vector<std::vector<uint8_t>> out;
    size_t si = 0, off = 0;
    while (off < stream.size()) {
        std::vector<uint8_t> p(188, 0xFF);
        while (si < starts.size() && starts[si] < off) si++;
        // a payload unit start needs the pointer byte: 183 bytes of data, else 184
        const bool start = si < starts.size() && starts[si] < off + 183;
        const size_t cap = start ? 183 : 184, take = std::min(cap, stream.size() - off);
        p[0] = 0x47; p[1] = (uint8_t)((start ? 0x40 : 0) | ((pid >> 8) & 0x1F)); p[2] = (uint8_t)pid; p[3] = (uint8_t)(0x10 | (cc & 0x0F));
        cc = (uint8_t)((cc + 1) & 0x0F);
        if (start) { p[4] = (uint8_t)(starts[si] - off); memcpy(&p[5], &stream[off], take); }
        else memcpy(&p[4], &stream[off], take);
        off += take;
        out.push_back(std::move(p));
    }
    return out;
}


// ---------------------------------------------------------------- multicast media

const char* rtpPayloadName(int pt) {
    switch (pt) {
    case 0: return "PCMU"; case 3: return "GSM"; case 8: return "PCMA"; case 9: return "G722"; case 10: return "L16 stereo"; case 11: return "L16 mono"; case 14: return "MPEG audio (MPA)";
    case 26: return "JPEG"; case 31: return "H.261"; case 32: return "MPEG video (MPV)"; case 33: return "MPEG-TS (MP2T)"; case 34: return "H.263"; default: return pt >= 96 ? "dynamic" : "";
    }
}

bool parseSdp(const std::string& text, SdpSession& o) {
    o = SdpSession();
    size_t pos = 0;
    bool any = false;
    SdpMedia* cur = nullptr;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() < 2 || line[1] != '=') continue;
        const char k = line[0];
        const std::string v = line.substr(2);
        if (k == 'v') any = true;
        else if (k == 's') o.name = v;
        else if (k == 'i' && !cur) o.info = v;
        else if (k == 'o') o.origin = v;
        else if (k == 'c') {
            const size_t a = v.find("IN IP");
            if (a != std::string::npos) {
                size_t b = v.find(' ', a + 3);
                std::string addr = b == std::string::npos ? "" : v.substr(b + 1);
                const size_t sl = addr.find('/');
                if (sl != std::string::npos) addr.resize(sl);
                if (cur) cur->address = addr; else o.address = addr;
            }
        } else if (k == 'm') {
            SdpMedia m;
            char type[32] = {}, proto[32] = {};
            int port = 0, used = 0;
            if (sscanf(v.c_str(), "%31s %d%n", type, &port, &used) >= 2) {
                m.type = type; m.port = port;
                size_t p2 = (size_t)used;
                while (p2 < v.size() && v[p2] == ' ') p2++;
                size_t q = v.find(' ', p2);
                m.proto = v.substr(p2, q == std::string::npos ? std::string::npos : q - p2);
                while (q != std::string::npos && q < v.size()) { while (q < v.size() && v[q] == ' ') q++; if (q >= v.size()) break; m.payloadTypes.push_back(atoi(v.c_str() + q)); q = v.find(' ', q); }
                o.media.push_back(m);
                cur = &o.media.back();
            }
        } else if (k == 'a' && cur) {
            if (v.rfind("rtpmap:", 0) == 0) { const int pt = atoi(v.c_str() + 7); const size_t sp = v.find(' '); if (sp != std::string::npos) cur->rtpmap[pt] = v.substr(sp + 1); }
            else if (v.rfind("control:", 0) == 0) cur->control = v.substr(8);
        }
    }
    return any && !o.name.empty();
}

bool IpMedia::udp(const IpInfo& info, const uint8_t* p, size_t n, double now, size_t& tsOffset) {
    tsOffset = 0;
    if (n == 0) return false;
    char key[96];
    snprintf(key, sizeof key, "%s:%d", addrString(info.dst, false).c_str(), info.dport);
    // SAP announcements: 224.2.127.254:9875 (and ff0e::2:7ffe)
    if (info.dport == 9875 && n > 8) {
        const uint8_t b0 = p[0];
        const bool v6 = (b0 & 0x10) != 0, del = (b0 & 0x04) != 0, enc = (b0 & 0x02) != 0, comp = (b0 & 0x01) != 0;
        size_t i = 4 + (v6 ? 16 : 4) + (size_t)p[1] * 4;
        if (!del && !enc && !comp && i < n) {
            std::string text((const char*)p + i, n - i);
            if (text.rfind("v=", 0) != 0) { const size_t z = text.find('\0'); if (z != std::string::npos) text = text.substr(z + 1); }   // the payload type string
            SdpSession s;
            if (parseSdp(text, s)) {
                const std::string id = s.origin + "|" + s.name;
                SdpSession& e = sdp_[id];
                const uint64_t seen = e.seen + 1;
                e = s; e.seen = seen; e.source = addrString(info.src, false);
                if (sdp_.size() > 512) sdp_.erase(sdp_.begin());
            }
        }
        F& f = flows_[key];
        f.f.dst = addrString(info.dst, false); f.f.src = addrString(info.src, false); f.f.port = info.dport; f.f.kind = "SAP";
        f.f.packets++; f.f.bytes += n;
        return false;
    }
    if (flows_.size() > 512 && !flows_.count(key)) return false;
    F& f = flows_[key];
    f.f.dst = addrString(info.dst, false); f.f.src = addrString(info.src, false); f.f.port = info.dport;
    f.f.packets++; f.f.bytes += n;
    if (f.t0 == 0) { f.t0 = now; f.winT = now; }
    f.tLast = now;
    f.bytesWin += n;
    if (now - f.winT >= 2.0) { f.f.kbps = (double)f.bytesWin * 8 / 1000 / (now - f.winT); f.bytesWin = 0; f.winT = now; }
    // MPEG-TS: whole transport packets, sync byte every 188 bytes
    auto tsAt = [&](size_t off) {
        if (n < off + 188 || (n - off) % 188 != 0) return false;
        for (size_t k = off; k < n; k += 188) if (p[k] != 0x47) return false;
        return true;
    };
    if (tsAt(0)) { f.f.kind = "MPEG-TS"; f.f.codec = "MPEG-TS"; tsOffset = 0; return true; }
    if (n >= 12 && (p[0] >> 6) == 2) {   // RTP
        const size_t cc = p[0] & 15;
        size_t off = 12 + cc * 4;
        if ((p[0] & 0x10) && n >= off + 4) off += 4 + (size_t)(p[off + 2] << 8 | p[off + 3]) * 4;   // header extension
        const int pt = p[1] & 0x7F;
        const uint16_t seq = (uint16_t)(p[2] << 8 | p[3]);
        const uint32_t ssrc = (uint32_t)p[8] << 24 | (uint32_t)p[9] << 16 | (uint32_t)p[10] << 8 | p[11];
        if (f.haveSeq) {
            const uint16_t d = (uint16_t)(seq - f.lastSeq);
            if (d == 1) f.goodSeq++;
            else if (d > 1 && d < 1000) { f.f.lost += (uint64_t)d - 1; f.goodSeq++; }
            else if (d != 0) f.goodSeq = 0;
        }
        f.lastSeq = seq; f.haveSeq = true;
        if (f.goodSeq >= 4 && off <= n) {
            f.f.kind = pt == 33 ? "RTP MPEG-TS" : "RTP";
            f.f.payloadType = pt; f.f.ssrc = ssrc;
            f.f.codec = rtpPayloadName(pt);
            if (pt == 33 && tsAt(off)) { tsOffset = off; return true; }
        }
    }
    return false;
}

std::vector<MediaFlow> IpMedia::flows() const {
    std::vector<MediaFlow> v;
    for (const auto& kv : flows_) {
        MediaFlow m = kv.second.f;
        if (m.kind.empty()) continue;
        // a dynamic payload type named by an announcement
        if (m.payloadType >= 96) for (const auto& s : sdp_) for (const auto& md : s.second.media) {
            auto it = md.rtpmap.find(m.payloadType);
            if ((md.address.empty() ? s.second.address : md.address) == m.dst && md.port == m.port && it != md.rtpmap.end()) m.codec = it->second;
        }
        v.push_back(m);
    }
    std::sort(v.begin(), v.end(), [](const MediaFlow& a, const MediaFlow& b) { return a.bytes > b.bytes; });
    return v;
}

std::vector<SdpSession> IpMedia::sessions() const {
    std::vector<SdpSession> v;
    for (const auto& kv : sdp_) v.push_back(kv.second);
    return v;
}

// ---------------------------------------------------------------- statistics, PCAP, sink

bool FlowKey::operator<(const FlowKey& o) const {
    if (v6 != o.v6) return v6 < o.v6;
    if (src != o.src) return src < o.src;
    if (dst != o.dst) return dst < o.dst;
    if (protocol != o.protocol) return protocol < o.protocol;
    if (sport != o.sport) return sport < o.sport;
    return dport < o.dport;
}

static const size_t kMaxFlows = 4096;

void IpStats::add(const IpInfo& p, int udpOk) {
    packets++; bytes += p.len;
    if (p.v6) ipv6++; else ipv4++;
    protocols[p.protocol]++;
    if (p.hasPorts) {
        if (p.protocol == kProtoUdp && udpPorts.size() < 8192) udpPorts[p.dport]++;
        else if (p.protocol == kProtoTcp && tcpPorts.size() < 8192) tcpPorts[p.dport]++;
    }
    FlowKey k;
    k.v6 = p.v6; k.src = p.src; k.dst = p.dst; k.protocol = p.protocol; k.sport = p.hasPorts ? p.sport : 0; k.dport = p.hasPorts ? p.dport : 0;
    if (flows_.size() >= kMaxFlows && !flows_.count(k)) {   // make room: drop the smallest quarter
        std::vector<uint64_t> sz;
        for (const auto& f : flows_) sz.push_back(f.second.bytes);
        std::sort(sz.begin(), sz.end());
        const uint64_t cut = sz[sz.size() / 4];
        for (auto it = flows_.begin(); it != flows_.end();) { if (it->second.bytes <= cut) it = flows_.erase(it); else ++it; }
    }
    Flow& f = flows_[k];
    f.key = k; f.packets++; f.bytes += p.len;
    if (udpOk >= 0) { f.udpChecked++; if (!udpOk) f.udpBad++; }
}

std::vector<Flow> IpStats::topFlows(size_t n) const {
    std::vector<Flow> v;
    for (const auto& f : flows_) v.push_back(f.second);
    std::sort(v.begin(), v.end(), [](const Flow& a, const Flow& b) { return a.bytes > b.bytes; });
    if (v.size() > n) v.resize(n);
    return v;
}

size_t IpStats::encryptedFlows() const {
    size_t n = 0;
    for (const auto& f : flows_) if (f.second.encrypted()) n++;
    return n;
}

bool PcapWriter::open(const std::string& p) {
    close();
    f_ = fopen(p.c_str(), "wb");
    if (!f_) return false;
    path = p; packets = bytes = 0;
    const wire::Bytes hdr = wire::pcapFileHeader(wire::kLinkRaw, 65535);
    fwrite(hdr.data(), 1, hdr.size(), f_);
    return true;
}

void PcapWriter::close() { if (f_) { fclose(f_); f_ = nullptr; } }

void PcapWriter::write(double t, const uint8_t* p, size_t n) {
    if (!f_) return;
    const size_t stored = std::min<size_t>(n, 65535);
    wire::Bytes rec;
    wire::appendPcapRecord(rec, (int64_t)(t * 1e6), p, stored);
    rec[12] = (uint8_t)n; rec[13] = (uint8_t)(n >> 8); rec[14] = (uint8_t)(n >> 16); rec[15] = (uint8_t)(n >> 24);   // the original length, when cut
    fwrite(rec.data(), 1, rec.size(), f_);
    packets++; bytes += n;
}

void IpSink::push(const uint8_t* ip, size_t n, double now) {
    IpInfo info;
    if (!parseIp(ip, n, info)) return;
    std::vector<uint8_t> whole;
    if (!frag.push(ip, info, whole)) return;
    IpInfo wi = info;
    if (whole.size() != info.len || memcmp(whole.data(), ip, info.len) != 0) { if (!parseIp(whole.data(), whole.size(), wi)) return; }
    stats.add(wi, udpChecksumOk(whole.data(), wi));
    if (onUdp && wi.protocol == kProtoUdp && !wi.v6) {
        const size_t ihl = (size_t)(whole[0] & 0x0F) * 4;
        if (((whole[6] << 8 | whole[7]) & 0x3FFF) == 0 && wi.len >= ihl + 8) {
            const size_t ulen = (size_t)(whole[ihl + 4] << 8 | whole[ihl + 5]);
            const size_t avail = wi.len - ihl - 8;
            onUdp(wi, whole.data(), whole.data() + ihl + 8, std::min(avail, ulen >= 8 ? ulen - 8 : avail), now);
        }
    }
    if (pcap.isOpen()) pcap.write(now, whole.data(), wi.len);
    if (wire::on(wire::DvbIp)) wire::Hub::get().publish(wire::DvbIp, whole.data(), wi.len);
}

} // namespace dvbsip
} // namespace dect2
