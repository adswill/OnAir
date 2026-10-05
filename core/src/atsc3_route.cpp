#include "dect2/atsc3_route.h"
#include "dect2/inflate.h"
#include "dect2/xmlmini.h"
#include <algorithm>
#include <cstdlib>
#include <tuple>

namespace dect2 {
namespace atsc3 {

namespace {
uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
void put32(std::vector<uint8_t>& v, uint32_t x) { v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x); }
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
    if (p.psi != 2 || p.version != 1 || hdrLen < 4 || (size_t)hdrLen > n) return false;   // ROUTE source packets: PSI = 10
    const int cciBytes = (c + 1) * 4, tsiBytes = s * 4 + h * 2, toiBytes = o * 4 + h * 2;
    int pos = 4;
    if (pos + cciBytes + tsiBytes + toiBytes > hdrLen) return false;
    pos += cciBytes;
    auto rd = [&](int bytes) { uint32_t v = 0; for (int i = 0; i < bytes; i++) v = (v << 8) | d[pos + i]; pos += bytes; return v; };
    p.tsi = tsiBytes ? rd(tsiBytes) : 0;
    p.toi = toiBytes ? rd(toiBytes) : 0;
    // header extensions up to the end of the header
    while (pos < hdrLen) {
        const int het = d[pos];
        if (het >= 128) {   // fixed length: one 32-bit word
            if (pos + 4 > hdrLen) return false;
            if (het == 194) p.transferLength = ((int64_t)d[pos + 1] << 16) | (d[pos + 2] << 8) | d[pos + 3];
            pos += 4;
        } else {
            if (pos + 2 > hdrLen) return false;
            const int words = d[pos + 1];
            if (words < 1 || pos + words * 4 > hdrLen) return false;
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

void RouteReceiver::push(const LctPacket& p, std::vector<RouteObject>& done) {
    counter_++;
    auto key = std::make_pair(p.tsi, p.toi);
    Open& o = open_[key];
    o.age = counter_;
    o.codePoint = p.codePoint;
    if (p.hasSct) { o.hasSct = true; o.sct = p.sct; }
    if (p.transferLength >= 0) o.length = p.transferLength;
    if (p.closeObject) o.length = (int64_t)p.startOffset + (int64_t)p.payload.size();
    if (!p.payload.empty()) {
        const uint32_t from = p.startOffset, to = p.startOffset + (uint32_t)p.payload.size();
        if (o.data.size() < to) o.data.resize(to);
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
        open_.erase(key);
    }
    if (open_.size() > 256) {   // objects that never completed
        for (auto it = open_.begin(); it != open_.end();) { if (counter_ - it->second.age > 5000) it = open_.erase(it); else ++it; }
    }
}

// ---- MIME

namespace {

std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
std::string trim(const std::string& s) { size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n"); return a == std::string::npos ? "" : s.substr(a, b - a + 1); }

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
    parseHeaders(s.substr(0, hend), top);
    auto ct = top.find("content-type");
    if (ct == top.end() || lower(ct->second).find("multipart/") != 0) return parts;
    size_t bp = lower(ct->second).find("boundary=");
    if (bp == std::string::npos) return parts;
    std::string boundary = trim(ct->second.substr(bp + 9));
    if (!boundary.empty() && boundary[0] == '"') { size_t q = boundary.find('"', 1); boundary = boundary.substr(1, q == std::string::npos ? std::string::npos : q - 1); }
    else { size_t q = boundary.find(';'); if (q != std::string::npos) boundary = trim(boundary.substr(0, q)); }
    const std::string delim = "--" + boundary;
    size_t pos = s.find(delim, hend + skip);
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
    if (!root || root->local() != "S-TSID") return false;
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

void RouteService::handleSls(const RouteObject& o) {
    // the SLS arrives as a multipart package (formatId 3); a bare XML document is accepted too
    auto parts = parseMultipart(o.data);
    std::vector<std::pair<std::string, std::string>> docs;   // content type, text
    if (parts.empty()) docs.push_back({"", std::string(o.data.begin(), o.data.end())});
    for (auto& p : parts) {
        std::string ct = p.headers.count("content-type") ? lower(p.headers["content-type"]) : "";
        std::vector<uint8_t> body = p.body;
        if (p.headers.count("content-encoding") && lower(p.headers["content-encoding"]).find("gzip") != std::string::npos) {
            std::vector<uint8_t> plain;
            if (gunzip(body.data(), body.size(), plain)) body.swap(plain);
        }
        docs.push_back({ct, std::string(body.begin(), body.end())});
    }
    for (auto& d : docs) {
        auto root = parseXml(d.second);
        if (!root) continue;
        const std::string n = root->local();
        if (n == "S-TSID") {
            Stsid st;
            if (!parseStsid(d.second, st)) continue;
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
        } else if (n == "MPD") mpd_ = d.second;
        else if (n == "UserServiceDescription" || n == "BundleDescription") usbd_ = d.second;
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
