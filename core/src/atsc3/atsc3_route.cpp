// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/diag.h"
#include "dect2/atsc3/atsc3_route.h"
#include "dect2/common/inflate.h"
#include "dect2/common/xmlmini.h"
#include <algorithm>
#include <cstdlib>
#include <tuple>
#include "dect2/common/str.h"
#include "dect2/common/endian.h"

namespace dect2 {
namespace atsc3 {

namespace {
uint32_t be32(const uint8_t* p) { return (uint32_t)dect2::endian::loadBe32(p); }
void put32(std::vector<uint8_t>& v, uint32_t x) { dect2::endian::putBe32(v, x); }
}

bool parseRoutePacket(const uint8_t* d, size_t n, LctPacket& p) {
    p = LctPacket();
    if (n < 4) return false;
    p.version = d[0] >> 4;
    const int c = (d[0] >> 2) & 3;
    p.psi = d[0] & 3;
    const int s = (d[1] >> 7) & 1, o = (d[1] >> 5) & 3, h = (d[1] >> 4) & 1;
    p.closeSession = (d[1] >> 1) & 1;
    p.closeObject = d[1] & 1;
    const int hdrLen = d[2] * 4;
    p.codePoint = d[3];
    // ROUTE source packets: the most significant bit of the PSI is 1 (A/331 A.3.4); the other bit is not defined there and is not checked
    if (!(p.psi & 2) || p.version != 1 || hdrLen < 4 || (size_t)hdrLen > n) {
        DIAG_DEBUG_EVERY(513579, 10.0, "A ROUTE packet of %zu bytes was dropped: it is not a ROUTE source packet of version 1 or its LCT header length (%d bytes) is invalid.", n, hdrLen);
        return false;
    }
    const int cciBytes = (c + 1) * 4, tsiBytes = s * 4 + h * 2, toiBytes = o * 4 + h * 2;
    int pos = 4;
    if (pos + cciBytes + tsiBytes + toiBytes > hdrLen) {
        DIAG_DEBUG_EVERY(360354, 10.0, "A ROUTE packet was dropped: its LCT header of %d bytes is too short for the congestion control, session and object identifiers it announces.", hdrLen);
        return false;
    }
    pos += cciBytes;
    auto rd = [&](int bytes) { uint32_t v = 0; for (int i = 0; i < bytes; i++) v = (v << 8) | d[pos + i]; pos += bytes; return v; };
    p.tsi = tsiBytes ? rd(tsiBytes) : 0;
    p.toi = toiBytes ? rd(toiBytes) : 0;
    // header extensions up to the end of the header
    while (pos < hdrLen) {
        const int het = d[pos];
        if (het >= 128) {   // fixed length: one 32-bit word
            if (pos + 4 > hdrLen) {
                DIAG_DEBUG_EVERY(425692, 10.0, "A ROUTE packet was dropped: a fixed-length LCT header extension (type %d) runs past the end of its header.", het);
                return false;
            }
            if (het == 194) p.transferLength = ((int64_t)d[pos + 1] << 16) | (d[pos + 2] << 8) | d[pos + 3];
            pos += 4;
        } else {
            if (pos + 2 > hdrLen) {
                DIAG_DEBUG_EVERY(479628, 10.0, "A ROUTE packet was dropped: a variable-length LCT header extension (type %d) starts past the end of its header.", het);
                return false;
            }
            const int words = d[pos + 1];
            if (words < 1 || pos + words * 4 > hdrLen) {
                DIAG_DEBUG_EVERY(180195, 10.0, "A ROUTE packet was dropped: a variable-length LCT header extension (type %d) of %d words does not fit in its header.", het, words);
                return false;
            }
            if (het == 67 && words >= 2) {   // EXT_TOL, 48 bit
                int64_t v = 0;
                for (int i = 0; i < 6; i++) v = (v << 8) | d[pos + 2 + i];
                p.transferLength = v;
            } else if (het == 2 && words >= 3) {   // EXT_TIME: flags, then SCT high/low when flagged
                int flags = (d[pos + 2] << 8) | d[pos + 3];
                int at = pos + 4;
                if ((flags & 0xC000) == 0xC000) {   // SCT-High and SCT-Low flags (RFC 5651)
                    if (at + 8 <= pos + words * 4) { p.sct = ((uint64_t)be32(d + at) << 32) | be32(d + at + 4); p.hasSct = true; }
                }
            }
            pos += words * 4;
        }
    }
    if (n < (size_t)hdrLen + 4) { p.startOffset = 0; return true; }   // no FEC payload id and no payload: a data-less packet
    p.startOffset = be32(d + hdrLen);
    p.payload.assign(d + hdrLen + 4, d + n);
    return true;
}

std::vector<uint8_t> makeRoutePacket(const LctPacket& p, bool withToL) {
    std::vector<uint8_t> v;
    std::vector<uint8_t> ext;
    if (withToL) {   // EXT_TOL, 24 bit (HET 194) when it fits, else 48 bit (HET 67)
        if (p.transferLength < (1 << 24)) { ext.push_back(194); ext.push_back(p.transferLength >> 16); ext.push_back(p.transferLength >> 8); ext.push_back(p.transferLength); }
        else { ext.push_back(67); ext.push_back(2); for (int i = 5; i >= 0; i--) ext.push_back(p.transferLength >> (8 * i)); }
    }
    if (p.hasSct) {   // EXT_TIME with SCT high and low
        ext.push_back(2); ext.push_back(3); ext.push_back(0xC0); ext.push_back(0x00);
        put32(ext, (uint32_t)(p.sct >> 32)); put32(ext, (uint32_t)p.sct);
    }
    const int hdrLen = 4 + 4 + 4 + 4 + (int)ext.size();   // fixed part, CCI, TSI, TOI, extensions
    v.push_back((uint8_t)(p.version << 4 | 0 << 2 | p.psi));
    v.push_back((uint8_t)(1 << 7 | 1 << 5 | (p.closeSession ? 2 : 0) | (p.closeObject ? 1 : 0)));   // S = 1, O = 01, H = 0
    v.push_back((uint8_t)(hdrLen / 4));
    v.push_back((uint8_t)p.codePoint);
    put32(v, 0);
    put32(v, p.tsi);
    put32(v, p.toi);
    v.insert(v.end(), ext.begin(), ext.end());
    put32(v, p.startOffset);
    v.insert(v.end(), p.payload.begin(), p.payload.end());
    return v;
}

namespace {
// whole ISO BMFF boxes from end to end
bool wholeBoxes(const std::vector<uint8_t>& d, size_t n) {
    size_t pos = 0;
    if (n < 8) return false;
    while (pos < n) {
        if (pos + 8 > n) return false;
        uint64_t size = be32(d.data() + pos);
        if (size == 1) { if (pos + 16 > n) return false; size = ((uint64_t)be32(d.data() + pos + 8) << 32) | be32(d.data() + pos + 12); }
        else if (size == 0) return true;   // to the end of the file
        if (size < 8 || pos + size > n) return false;
        pos += (size_t)size;
    }
    return pos == n;
}
}

void RouteReceiver::push(const LctPacket& p, std::vector<RouteObject>& done) {
    // offsets and lengths come from the air: the end of the payload is computed in 64 bits (a 32-bit sum wraps and the copy below would
    // write before the buffer it sized), and an object larger than any ATSC 3.0 object is not taken (fuzz_parsers)
    const uint64_t end = (uint64_t)p.startOffset + p.payload.size();
    if (end > kMaxObjectBytes || p.transferLength > (int64_t)kMaxObjectBytes) {
        DIAG_DEBUG_EVERY(438644, 10.0, "A ROUTE object of TSI %u is larger than the %llu bytes this receiver accepts, so its packets are dropped.", p.tsi, (unsigned long long)kMaxObjectBytes);
        return;
    }
    counter_++;
    auto key = std::make_pair(p.tsi, p.toi);
    auto drop = [&](std::map<std::pair<uint32_t, uint32_t>, Open>::iterator it) { openBytes_ -= it->second.data.size(); return open_.erase(it); };
    if (!open_.count(key) && p.startOffset == 0) {   // a new object: older ones of the TSI without a signalled length may have ended
        for (auto it = open_.begin(); it != open_.end();) {
            Open& q = it->second;
            if (it->first.first == p.tsi && q.length < 0 && q.ranges.size() == 1 && q.ranges[0].first == 0 && wholeBoxes(q.data, q.ranges[0].second)) {
                RouteObject r;
                r.tsi = it->first.first; r.toi = it->first.second; r.codePoint = q.codePoint; r.hasSct = q.hasSct; r.sct = q.sct;
                r.data.assign(q.data.begin(), q.data.begin() + q.ranges[0].second);
                done.push_back(std::move(r));
                it = drop(it);
            } else ++it;
        }
    }
    // room for this packet among the unfinished objects: the oldest other ones go first
    while (!p.payload.empty() && openBytes_ + end > kMaxOpenBytes && !open_.empty()) {
        auto oldest = open_.end();
        for (auto it = open_.begin(); it != open_.end(); ++it)
            if (it->first != key && (oldest == open_.end() || it->second.age < oldest->second.age)) oldest = it;
        if (oldest == open_.end()) break;
        drop(oldest);
    }
    Open& o = open_[key];
    o.age = counter_;
    o.codePoint = p.codePoint;
    if (p.hasSct) { o.hasSct = true; o.sct = p.sct; }
    if (p.transferLength >= 0) o.length = p.transferLength;
    if (p.closeObject) o.length = (int64_t)end;
    if (!p.payload.empty()) {
        const uint32_t from = p.startOffset, to = (uint32_t)end;
        if (o.data.size() < to) { openBytes_ += to - o.data.size(); o.data.resize(to); }
        std::copy(p.payload.begin(), p.payload.end(), o.data.begin() + from);
        o.ranges.push_back({from, to});
        std::sort(o.ranges.begin(), o.ranges.end());
        std::vector<std::pair<uint32_t, uint32_t>> m;
        for (auto& r : o.ranges) {
            if (!m.empty() && r.first <= m.back().second) m.back().second = std::max(m.back().second, r.second);
            else m.push_back(r);
        }
        o.ranges.swap(m);
    }
    if (o.length >= 0 && o.ranges.size() == 1 && o.ranges[0].first == 0 && (int64_t)o.ranges[0].second >= o.length) {
        RouteObject r;
        r.tsi = p.tsi; r.toi = p.toi; r.codePoint = o.codePoint; r.hasSct = o.hasSct; r.sct = o.sct;
        r.data.assign(o.data.begin(), o.data.begin() + o.length);
        done.push_back(std::move(r));
        drop(open_.find(key));
    }
    if (open_.size() > 256) {   // objects that never completed
        for (auto it = open_.begin(); it != open_.end();) { if (counter_ - it->second.age > 5000) it = drop(it); else ++it; }
    }
}

// ---- MIME

namespace {

std::string lower(std::string s) { return dect2::str::lower(std::move(s)); }
std::string trim(const std::string& s) { return dect2::str::trimChars(s, " \t\r\n"); }

void parseHeaders(const std::string& block, std::map<std::string, std::string>& h) {
    size_t pos = 0;
    std::string cur;
    while (pos <= block.size()) {
        size_t e = block.find('\n', pos);
        std::string line = block.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t') && !cur.empty()) h[cur] += " " + trim(line);
        else {
            size_t c = line.find(':');
            if (c != std::string::npos) { cur = lower(trim(line.substr(0, c))); h[cur] = trim(line.substr(c + 1)); }
        }
        if (e == std::string::npos) break;
        pos = e + 1;
    }
}

} // namespace

std::vector<MimePart> parseMultipart(const std::vector<uint8_t>& doc) {
    std::vector<MimePart> parts;
    std::string s(doc.begin(), doc.end());
    size_t hend = s.find("\r\n\r\n");
    size_t skip = 4;
    if (hend == std::string::npos) { hend = s.find("\n\n"); skip = 2; }
    if (hend == std::string::npos) return parts;
    std::map<std::string, std::string> top;
    std::string boundary;
    size_t from = hend + skip;
    if (s.compare(0, 2, "--") == 0) {   // no header block: the document starts with its first delimiter line
        size_t le = s.find('\n');
        if (le == std::string::npos) return parts;
        boundary = trim(s.substr(2, le - 2));
        from = 0;
    } else {
        parseHeaders(s.substr(0, hend), top);
        auto ct = top.find("content-type");
        if (ct == top.end() || lower(ct->second).find("multipart/") != 0) return parts;
        size_t bp = lower(ct->second).find("boundary=");
        if (bp == std::string::npos) return parts;
        boundary = trim(ct->second.substr(bp + 9));
        if (!boundary.empty() && boundary[0] == '"') { size_t q = boundary.find('"', 1); boundary = boundary.substr(1, q == std::string::npos ? std::string::npos : q - 1); }
        else { size_t q = boundary.find(';'); if (q != std::string::npos) boundary = trim(boundary.substr(0, q)); }
    }
    if (boundary.empty()) return parts;
    const std::string delim = "--" + boundary;
    size_t pos = s.find(delim, from);
    while (pos != std::string::npos) {
        size_t after = pos + delim.size();
        if (s.compare(after, 2, "--") == 0) break;   // closing delimiter
        size_t lineEnd = s.find('\n', after);
        if (lineEnd == std::string::npos) break;
        size_t start = lineEnd + 1;
        size_t next = s.find("\n" + delim, start);
        if (next == std::string::npos) break;
        size_t end = next;
        if (end > start && s[end - 1] == '\r') end--;
        std::string section = s.substr(start, end - start);
        MimePart part;
        size_t he = section.find("\r\n\r\n"), hs = 4;
        if (he == std::string::npos) { he = section.find("\n\n"); hs = 2; }
        if (section.compare(0, 2, "\r\n") == 0 || section.compare(0, 1, "\n") == 0) { he = 0; hs = section[0] == '\r' ? 2 : 1; }   // no headers
        if (he == std::string::npos) { part.body.assign(section.begin(), section.end()); }
        else {
            parseHeaders(section.substr(0, he), part.headers);
            part.body.assign(section.begin() + he + hs, section.end());
        }
        parts.push_back(std::move(part));
        pos = next + 1;
    }
    return parts;
}

std::vector<uint8_t> makeMultipart(const std::vector<MimePart>& parts, const std::string& boundary) {
    std::string s = "Content-Type: multipart/related; boundary=\"" + boundary + "\"\r\n\r\n";
    for (auto& p : parts) {
        s += "--" + boundary + "\r\n";
        for (auto& h : p.headers) s += h.first + ": " + h.second + "\r\n";
        s += "\r\n";
        s.append(p.body.begin(), p.body.end());
        s += "\r\n";
    }
    s += "--" + boundary + "--\r\n";
    return std::vector<uint8_t>(s.begin(), s.end());
}

// ---- S-TSID

bool parseStsid(const std::string& xml, Stsid& out) {
    auto root = parseXml(xml);
    if (!root || root->local() != "S-TSID") {
        DIAG_DEBUG_EVERY(993616, 10.0, "An ATSC 3.0 signalling document was expected to be an S-TSID but is not valid XML or has another root element, so it is ignored.");
        return false;
    }
    out = Stsid();
    for (auto& rs : root->all("RS")) {
        StsidSession s;
        ipFromString(rs->attr("sIpAddr"), s.srcIp);
        ipFromString(rs->attr("dIpAddr"), s.dstIp);
        s.dstPort = atoi(rs->attr("dPort", "0").c_str());
        for (auto& ls : rs->all("LS")) {
            StsidChannel c;
            c.tsi = (uint32_t)strtoul(ls->attr("tsi", "0").c_str(), nullptr, 0);
            if (auto sf = ls->first("SrcFlow")) {
                c.realTime = sf->attr("rt") == "true" || sf->attr("rt") == "1";
                if (auto ci = sf->first("ContentInfo"))
                    if (auto mi = ci->first("MediaInfo")) {
                        c.repId = mi->attr("repId");
                        c.contentType = mi->attr("contentType");
                        c.lang = mi->attr("lang");
                        c.startup = mi->attr("startup") == "true" || mi->attr("startup") == "1";
                    }
                for (auto& pl : sf->all("Payload")) {
                    StsidPayload p;
                    p.codePoint = atoi(pl->attr("codePoint", "0").c_str());
                    p.formatId = atoi(pl->attr("formatId", "0").c_str());
                    p.frag = atoi(pl->attr("frag", "0").c_str());
                    p.order = pl->attr("order") == "true" || pl->attr("order") == "1";
                    c.payloads.push_back(p);
                }
            }
            s.channels.push_back(c);
        }
        out.sessions.push_back(s);
    }
    return !out.sessions.empty();
}

// ---- service

RouteService::RouteService(uint32_t src, uint32_t dst, int port) : slsSrc_(src), slsDst_(dst), slsPort_(port) {}

namespace {
bool isGzip(const std::vector<uint8_t>& b) { return b.size() >= 18 && b[0] == 0x1F && b[1] == 0x8B; }
// The documents of an SLS delivery object (A/331 7.1.6.1, A.3.3.4, A.3.3.5): a multipart/related package, which may be gzip compressed as a
// whole; in Signed Package Mode a multipart/signed whose first part is that package and whose last is the signature; a part may carry its own
// Content-Encoding: gzip. A bare XML document is accepted too. Nested packages are opened down to a few levels.
void slsDocuments(std::vector<uint8_t> data, int depth, std::vector<std::string>& docs) {
    if (depth > 4) {
        DIAG_DEBUG_EVERY(473433, 10.0, "An ATSC 3.0 service signalling package is nested more than 4 levels deep, so the inner documents are ignored.");
        return;
    }
    if (isGzip(data)) { std::vector<uint8_t> plain; if (gunzip(data.data(), data.size(), plain)) data.swap(plain);
        else {
            DIAG_DEBUG_EVERY(922364, 10.0, "A gzip-compressed ATSC 3.0 service signalling object of %zu bytes could not be decompressed, so it is ignored.", data.size());
            return;
        }
    }
    auto parts = parseMultipart(data);
    if (parts.empty()) { docs.emplace_back(data.begin(), data.end()); return; }
    for (auto& p : parts) {
        const std::string ct = p.headers.count("content-type") ? lower(p.headers["content-type"]) : "";
        std::vector<uint8_t> body = std::move(p.body);
        const bool gz = (p.headers.count("content-encoding") && lower(p.headers["content-encoding"]).find("gzip") != std::string::npos) || isGzip(body);
        if (gz) { std::vector<uint8_t> plain; if (gunzip(body.data(), body.size(), plain)) body.swap(plain); }
        if (ct.compare(0, 10, "multipart/") == 0) {   // a package inside (the signed content of multipart/signed): give it its header back
            const std::string h = "Content-Type: " + p.headers["content-type"] + "\r\n\r\n";
            std::vector<uint8_t> inner(h.begin(), h.end());
            inner.insert(inner.end(), body.begin(), body.end());
            slsDocuments(std::move(inner), depth + 1, docs);
            continue;
        }
        if (ct.find("pkcs7") != std::string::npos || ct.find("signature") != std::string::npos) continue;   // the signature (not checked)
        if (isGzip(body) || (!body.empty() && body[0] == '-')) { slsDocuments(std::move(body), depth + 1, docs); continue; }
        docs.emplace_back(body.begin(), body.end());
    }
}
} // namespace

void RouteService::handleSls(const RouteObject& o) {
    slsObjects_++;
    std::vector<std::string> docs;
    slsDocuments(o.data, 0, docs);
    for (auto& d : docs) {
        auto root = parseXml(d);
        if (!root) continue;
        const std::string n = root->local();
        if (n == "S-TSID") {
            Stsid st;
            if (!parseStsid(d, st)) {
                DIAG_DEBUG_EVERY(280667, 10.0, "The S-TSID of the selected ATSC 3.0 service has no ROUTE sessions, so its components are not updated.");
                continue;
            }
            slsTables_++;
            stsid_ = st;
            haveStsid_ = true;
            components_.clear();
            for (auto& s : st.sessions) {
                for (auto& c : s.channels) {
                    RouteComponent rc;
                    rc.dstIp = s.dstIp ? s.dstIp : slsDst_;
                    rc.dstPort = s.dstPort ? s.dstPort : slsPort_;
                    rc.tsi = c.tsi; rc.repId = c.repId; rc.contentType = c.contentType; rc.lang = c.lang; rc.realTime = c.realTime;
                    components_.push_back(rc);
                }
            }
        } else if (n == "MPD") { mpd_ = d; slsTables_++; }
        else if (n == "UserServiceDescription" || n == "BundleDescription") { usbd_ = d; slsTables_++; }
    }
}

bool RouteService::push(const UdpDatagram& d) {
    bool sls = d.dstIp == slsDst_ && d.dstPort == slsPort_;
    bool known = false;
    for (auto& c : components_) if (c.dstIp == d.dstIp && c.dstPort == d.dstPort) known = true;
    if (!sls && !known) return false;
    LctPacket p;
    if (!parseRoutePacket(d.payload.data(), d.payload.size(), p)) return true;
    auto& r = rx_[std::make_tuple(d.dstIp, d.dstPort, p.tsi)];
    if (!r) r.reset(new RouteReceiver());
    std::vector<RouteObject> done;
    r->push(p, done);
    for (auto& o : done) {
        if (sls && o.tsi == 0) { handleSls(o); continue; }
        for (auto& c : components_) {
            if (c.dstIp == d.dstIp && c.dstPort == d.dstPort && c.tsi == o.tsi) { if (onObject) onObject(c, o); break; }
        }
    }
    return true;
}

} // namespace atsc3
} // namespace dect2
