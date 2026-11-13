// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/diag.h"
#include "dect2/atsc3/atsc3_ip.h"
#include "dect2/common/inflate.h"
#include "dect2/common/xmlmini.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace dect2 {
namespace atsc3 {

std::string ipToString(uint32_t ip) {
    char b[24];
    snprintf(b, sizeof b, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return b;
}

bool ipFromString(const std::string& s, uint32_t& ip) {
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return false;
    ip = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

namespace {
uint16_t checksum(const uint8_t* d, int n) {
    uint32_t s = 0;
    for (int i = 0; i + 1 < n; i += 2) s += (d[i] << 8) | d[i + 1];
    if (n & 1) s += d[n - 1] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}
}

bool IpReceiver::push(const uint8_t* p, size_t n, UdpDatagram& out) {
    counter_++;
    if (n < 20 || (p[0] >> 4) != 4) {
        DIAG_DEBUG_EVERY(913369, 10.0, "An ATSC 3.0 IP packet of %zu bytes is not IPv4, so it is dropped.", n);
        dropped_++;
        return false;
    }
    const int ihl = (p[0] & 15) * 4;
    const int total = (p[2] << 8) | p[3];
    if (ihl < 20 || (size_t)ihl > n || total < ihl || (size_t)total > n) {
        DIAG_DEBUG_EVERY(595119, 10.0, "An ATSC 3.0 IPv4 packet has an inconsistent header length (%d) or total length (%d of %zu bytes), so it is dropped.", ihl, total, n);
        dropped_++;
        return false;
    }
    if (p[9] != 17) {   // UDP only
        DIAG_DEBUG_EVERY(502789, 10.0, "An ATSC 3.0 IPv4 packet carries protocol %d instead of UDP, so it is dropped.", (int)p[9]);
        dropped_++;
        return false;
    }
    const int fragField = (p[6] << 8) | p[7];
    const bool more = fragField & 0x2000;
    const int off = (fragField & 0x1FFF) * 8;
    const uint32_t src = (p[12] << 24) | (p[13] << 16) | (p[14] << 8) | p[15], dst = (p[16] << 24) | (p[17] << 16) | (p[18] << 8) | p[19];
    const uint8_t* body = p + ihl;
    int blen = total - ihl;
    std::vector<uint8_t> whole;
    if (off == 0 && !more) {
        whole.assign(body, body + blen);
    } else {
        char key[64];
        snprintf(key, sizeof key, "%08x-%08x-%02x%02x-%d", src, dst, p[4], p[5], p[9]);
        Frag& f = frags_[key];
        f.age = counter_;
        if ((int)f.data.size() < off + blen) f.data.resize(off + blen);
        std::copy(body, body + blen, f.data.begin() + off);
        f.got.push_back({off, off + blen});
        if (!more) f.total = off + blen;
        if (f.total >= 0) {
            std::sort(f.got.begin(), f.got.end());
            int cover = 0;
            for (auto& g : f.got) { if (g.first > cover) break; cover = std::max(cover, g.second); }
            if (cover >= f.total) { whole.assign(f.data.begin(), f.data.begin() + f.total); frags_.erase(key); }
        }
        for (auto it = frags_.begin(); it != frags_.end();) { if (counter_ - it->second.age > 2000) it = frags_.erase(it); else ++it; }   // lost fragments
        if (whole.empty()) return false;
    }
    if (whole.size() < 8) {
        DIAG_DEBUG_EVERY(836372, 10.0, "An ATSC 3.0 UDP datagram of %zu bytes is shorter than its 8 byte header, so it is dropped.", whole.size());
        dropped_++;
        return false;
    }
    const int ulen = (whole[4] << 8) | whole[5];
    if (ulen < 8 || (size_t)ulen > whole.size()) {
        DIAG_DEBUG_EVERY(607412, 10.0, "An ATSC 3.0 UDP datagram announces a length of %d bytes but only %zu arrived, so it is dropped.", ulen, whole.size());
        dropped_++;
        return false;
    }
    out.srcIp = src; out.dstIp = dst;
    out.srcPort = (whole[0] << 8) | whole[1];
    out.dstPort = (whole[2] << 8) | whole[3];
    out.payload.assign(whole.begin() + 8, whole.begin() + ulen);
    return true;
}

std::vector<std::vector<uint8_t>> makeUdpPackets(uint32_t src, uint32_t dst, int sport, int dport, const std::vector<uint8_t>& payload, int id, int mtu) {
    std::vector<uint8_t> udp;
    int ulen = 8 + (int)payload.size();
    udp.push_back(sport >> 8); udp.push_back(sport); udp.push_back(dport >> 8); udp.push_back(dport);
    udp.push_back(ulen >> 8); udp.push_back(ulen); udp.push_back(0); udp.push_back(0);
    udp.insert(udp.end(), payload.begin(), payload.end());
    std::vector<std::vector<uint8_t>> out;
    int maxData = mtu > 0 ? (mtu - 20) / 8 * 8 : (int)udp.size();
    if (maxData <= 0) maxData = (int)udp.size();
    for (int off = 0; off < (int)udp.size(); off += maxData) {
        int n = std::min<int>(maxData, (int)udp.size() - off);
        bool more = off + n < (int)udp.size();
        std::vector<uint8_t> h(20, 0);
        h[0] = 0x45; h[2] = (20 + n) >> 8; h[3] = (20 + n) & 255; h[4] = id >> 8; h[5] = id & 255;
        int ff = (more ? 0x2000 : 0) | (off / 8);
        h[6] = ff >> 8; h[7] = ff & 255; h[8] = 64; h[9] = 17;
        h[12] = src >> 24; h[13] = src >> 16; h[14] = src >> 8; h[15] = src;
        h[16] = dst >> 24; h[17] = dst >> 16; h[18] = dst >> 8; h[19] = dst;
        uint16_t c = checksum(h.data(), 20);
        h[10] = c >> 8; h[11] = c & 255;
        h.insert(h.end(), udp.begin() + off, udp.begin() + off + n);
        out.push_back(h);
    }
    return out;
}

// ---- LLS

namespace {

int toInt(const std::string& s, int def = 0) { return s.empty() ? def : (int)strtol(s.c_str(), nullptr, 0); }

void parseSlt(const std::shared_ptr<XmlNode>& root, Slt& slt) {
    for (size_t i = 0, st = 0; i <= root->attr("bsid").size(); i++) {
        std::string s = root->attr("bsid");
        if (i == s.size() || s[i] == ' ') { if (i > st) slt.bsid.push_back(toInt(s.substr(st, i - st))); st = i + 1; }
    }
    for (auto& sv : root->all("Service")) {
        SltService s;
        s.serviceId = toInt(sv->attr("serviceId"));
        s.majorChannel = toInt(sv->attr("majorChannelNo"));
        s.minorChannel = toInt(sv->attr("minorChannelNo"));
        s.category = toInt(sv->attr("serviceCategory"));
        s.shortName = sv->attr("shortServiceName");
        s.globalId = sv->attr("globalServiceID");
        s.hidden = sv->attr("hidden") == "true" || sv->attr("hidden") == "1";
        s.protectedService = sv->attr("protected") == "true" || sv->attr("protected") == "1";
        s.seqNum = toInt(sv->attr("sltSvcSeqNum"));
        if (auto b = sv->first("BroadcastSvcSignaling")) {
            s.slsProtocol = toInt(b->attr("slsProtocol"));
            ipFromString(b->attr("slsDestinationIpAddress"), s.slsDstIp);
            ipFromString(b->attr("slsSourceIpAddress"), s.slsSrcIp);
            s.slsDstPort = toInt(b->attr("slsDestinationUdpPort"));
        }
        slt.services.push_back(s);
    }
}

} // namespace

std::vector<AeatAlert> parseAeat(const std::string& xml) {
    std::vector<AeatAlert> out;
    auto root = parseXml(xml);
    if (!root || root->local() != "AEAT") return out;
    for (const auto& ev : root->all("AEA")) {
        AeatAlert a;
        a.id = ev->attr("AEAId"); a.issuer = ev->attr("issuer"); a.audience = ev->attr("audience"); a.type = ev->attr("AEAType"); a.priority = ev->attr("priority");
        if (auto h = ev->first("Header")) {
            a.effective = h->attr("effective"); a.expires = h->attr("expires");
            if (auto c = h->first("EventCode")) a.eventCode = c->text;
            if (auto d = h->first("EventDesc")) a.eventDesc = d->text;
            for (const auto& l : h->all("Location")) { if (!a.location.empty()) a.location += "; "; a.location += l->text; }
        }
        auto texts = ev->all("AEAText");
        if (!texts.empty()) { a.text = texts[0]->text; a.lang = texts[0]->attr("lang"); }
        out.push_back(std::move(a));
    }
    return out;
}

namespace {
// one LLS table body (gzip compressed XML for the ids 1 to 5)
bool llsBody(int tableId, const uint8_t* p, size_t n, LlsTable& t) {
    if (tableId < 1 || tableId > 5) {
        DIAG_DEBUG_EVERY(804564, 10.0, "An ATSC 3.0 low level signalling table has the unknown id %d, so it is ignored.", tableId);
        return false;
    }
    std::vector<uint8_t> x;
    if (!gunzip(p, n, x, 8u << 20)) {
        DIAG_DEBUG_EVERY(530415, 10.0, "The gzip data of ATSC 3.0 low level signalling table %d (%zu bytes) could not be decompressed, so the table is ignored.", tableId, n);
        return false;
    }
    t.xml.assign(x.begin(), x.end());
    if (tableId == 1) {
        auto root = parseXml(t.xml);
        if (!root || root->local() != "SLT") {
            DIAG_DEBUG_EVERY(314962, 10.0, "An ATSC 3.0 service list table is not valid XML or has another root element, so the service list is not updated.");
            return false;
        }
        parseSlt(root, t.slt);
    }
    t.ok = true;
    return true;
}
} // namespace

std::vector<LlsTable> parseLlsAll(const uint8_t* p, size_t n) {
    std::vector<LlsTable> all;
    if (n < 4) return all;
    LlsTable h;
    h.tableId = p[0]; h.groupId = p[1]; h.groupCount = p[2] + 1; h.version = p[3];
    if (h.tableId != 0xFE) {
        LlsTable t = h;
        if (llsBody(t.tableId, p + 4, n - 4, t)) all.push_back(std::move(t));
        return all;
    }
    // SignedMultiTable: LLS_payload_count, then id, version, 16-bit length and the payload for each, then the signature
    size_t pos = 4;
    if (pos >= n) return all;
    const int count = p[pos++];
    for (int i = 0; i < count; i++) {
        if (pos + 4 > n) break;
        LlsTable t = h;
        t.tableId = p[pos]; t.version = p[pos + 1];
        const size_t len = ((size_t)p[pos + 2] << 8) | p[pos + 3];
        pos += 4;
        if (pos + len > n) break;
        if (llsBody(t.tableId, p + pos, len, t)) all.push_back(std::move(t));
        pos += len;
    }
    return all;
}

LlsTable parseLls(const uint8_t* p, size_t n) {
    auto all = parseLlsAll(p, n);
    for (auto& t : all) if (t.tableId == 1) return t;
    if (!all.empty()) return all[0];
    LlsTable t;
    if (n >= 4) { t.tableId = p[0]; t.groupId = p[1]; t.groupCount = p[2] + 1; t.version = p[3]; }
    return t;
}

std::vector<uint8_t> gzipStored(const uint8_t* data, size_t size) {
    std::vector<uint8_t> g = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 3};
    size_t pos = 0;
    do {
        const size_t n = std::min<size_t>(65535, size - pos);
        const bool last = pos + n == size;
        g.push_back(last ? 1 : 0);
        g.push_back(n & 255); g.push_back(n >> 8); g.push_back(~n & 255); g.push_back((~n >> 8) & 255);
        g.insert(g.end(), data + pos, data + pos + n);
        pos += n;
    } while (pos < size);
    const uint32_t crc = crc32Ieee(data, size);
    for (int i = 0; i < 4; i++) g.push_back((crc >> (8 * i)) & 255);
    for (int i = 0; i < 4; i++) g.push_back((size >> (8 * i)) & 255);
    return g;
}

std::vector<uint8_t> makeLls(int id, int group, int version, const std::string& xml) {
    std::vector<uint8_t> v = {(uint8_t)id, (uint8_t)group, 0, (uint8_t)version};
    const auto g = gzipStored((const uint8_t*)xml.data(), xml.size());
    v.insert(v.end(), g.begin(), g.end());
    return v;
}

std::vector<uint8_t> makeSignedMultiLls(int group, const std::vector<std::tuple<int, int, std::string>>& tables, int sigBytes) {
    std::vector<uint8_t> v = {0xFE, (uint8_t)group, 0, 1, (uint8_t)tables.size()};
    for (auto& t : tables) {
        const std::string& xml = std::get<2>(t);
        const auto g = gzipStored((const uint8_t*)xml.data(), xml.size());
        v.push_back((uint8_t)std::get<0>(t)); v.push_back((uint8_t)std::get<1>(t));
        v.push_back((uint8_t)(g.size() >> 8)); v.push_back((uint8_t)g.size());
        v.insert(v.end(), g.begin(), g.end());
    }
    v.push_back((uint8_t)(sigBytes >> 8)); v.push_back((uint8_t)sigBytes);
    for (int i = 0; i < sigBytes; i++) v.push_back((uint8_t)(0x30 + i));
    return v;
}

} // namespace atsc3
} // namespace dect2
