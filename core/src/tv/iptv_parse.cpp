// Internet TV: URLs, M3U playlists, XMLTV guides, HLS playlists. Pure text handling, no network (see dect2/tv/iptv.h).
#include "dect2/tv/iptv.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>

namespace dect2::iptv {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}
static std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
static bool startsWith(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }
static bool istarts(const std::string& s, const char* p) { size_t n = strlen(p); if (s.size() < n) return false; for (size_t i = 0; i < n; i++) if (std::tolower((unsigned char)s[i]) != p[i]) return false; return true; }

// ---------------- URLs ----------------
Url parseUrl(const std::string& in) {
    Url u;
    const std::string s = trim(in);
    const size_t se = s.find("://");
    if (se == std::string::npos || se == 0) return u;
    u.scheme = lower(s.substr(0, se));
    for (char c : u.scheme) if (!(std::isalnum((unsigned char)c) || c == '+' || c == '-' || c == '.')) return u;
    size_t i = se + 3;
    size_t pe = s.find_first_of("/?#", i);
    std::string auth = s.substr(i, pe == std::string::npos ? std::string::npos : pe - i);
    u.path = pe == std::string::npos ? "/" : s.substr(pe);
    const size_t h = u.path.find('#');
    if (h != std::string::npos) u.path.resize(h);
    if (u.path.empty() || u.path[0] != '/') u.path = "/" + u.path;
    const size_t at = auth.rfind('@');
    if (at != std::string::npos) { u.user = auth.substr(0, at); auth = auth.substr(at + 1); }
    if (!auth.empty() && auth[0] == '[') {
        const size_t rb = auth.find(']');
        if (rb == std::string::npos) return u;
        u.host = auth.substr(1, rb - 1); u.ipv6 = true;
        if (rb + 1 < auth.size() && auth[rb + 1] == ':') u.port = atoi(auth.c_str() + rb + 2);
    } else {
        const size_t c = auth.rfind(':');
        if (c != std::string::npos) { u.port = atoi(auth.c_str() + c + 1); u.host = auth.substr(0, c); }
        else u.host = auth;
    }
    if (u.host.empty()) return u;
    if (u.port < 0 || u.port > 65535) return u;
    if (u.port == 0) u.port = u.scheme == "https" ? 443 : u.scheme == "http" ? 80 : u.scheme == "rtsp" ? 554 : 0;
    u.ok = true;
    return u;
}

static std::string origin(const Url& u) {
    std::string o = u.scheme + "://";
    if (!u.user.empty()) o += u.user + "@";
    o += u.ipv6 ? "[" + u.host + "]" : u.host;
    const int def = u.scheme == "https" ? 443 : u.scheme == "http" ? 80 : u.scheme == "rtsp" ? 554 : 0;
    if (u.port && u.port != def) o += ":" + std::to_string(u.port);
    return o;
}

static std::string removeDots(const std::string& path) {
    std::string q, frag;
    std::string p = path;
    const size_t qm = p.find('?');
    if (qm != std::string::npos) { q = p.substr(qm); p.resize(qm); }
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        const std::string seg = p.substr(i, j - i);
        if (seg == "..") { if (out.size() > 1) out.pop_back(); }
        else if (seg != ".") out.push_back(seg);
        if (j == p.size()) break;
        i = j + 1;
    }
    std::string r;
    for (size_t k = 0; k < out.size(); k++) { if (k) r += "/"; r += out[k]; }
    if (r.empty() || r[0] != '/') r = "/" + r;
    // a trailing "." or ".." means a directory
    if (p.size() >= 2 && (p.compare(p.size() - 2, 2, "/.") == 0 || p.compare(p.size() - 3 > p.size() ? 0 : p.size() - 3, 3, "/..") == 0) && r.back() != '/') r += "/";
    return r + q;
}

std::string resolveUrl(const std::string& base, const std::string& refIn) {
    const std::string ref = trim(refIn);
    if (ref.empty()) return base;
    if (parseUrl(ref).ok) return ref;
    const Url b = parseUrl(base);
    if (!b.ok) return ref;
    if (startsWith(ref, "//")) return b.scheme + ":" + ref;
    if (ref[0] == '/') return origin(b) + removeDots(ref);
    if (ref[0] == '?') { std::string p = b.path; const size_t q = p.find('?'); if (q != std::string::npos) p.resize(q); return origin(b) + p + ref; }
    std::string dir = b.path;
    const size_t q = dir.find('?'); if (q != std::string::npos) dir.resize(q);
    const size_t sl = dir.rfind('/');
    dir = sl == std::string::npos ? "/" : dir.substr(0, sl + 1);
    return origin(b) + removeDots(dir + ref);
}

bool looksLikeStreamUrl(const std::string& s) {
    const Url u = parseUrl(s);
    if (!u.ok) return false;
    static const char* ok[] = {"http", "https", "udp", "rtp", "rtsp", "rtsps", "mms", "mmsh", "srt", "rtmp", "rtmps"};
    for (const char* k : ok) if (u.scheme == k) return true;
    return false;
}

std::string redactUrl(const std::string& s) {
    std::string r = s;
    const size_t se = r.find("://");
    if (se != std::string::npos) {
        const size_t pe = r.find_first_of("/?#", se + 3);
        const size_t at = r.find('@', se + 3);
        if (at != std::string::npos && (pe == std::string::npos || at < pe)) r.erase(se + 3, at - se - 3 + 1);
    }
    const size_t q = r.find('?');
    if (q != std::string::npos) {
        std::string head = r.substr(0, q), tail = r.substr(q + 1), out;
        size_t i = 0;
        while (i <= tail.size()) {
            size_t j = tail.find('&', i);
            if (j == std::string::npos) j = tail.size();
            std::string kv = tail.substr(i, j - i);
            const size_t eq = kv.find('=');
            if (eq != std::string::npos) {
                const std::string k = lower(kv.substr(0, eq));
                if (k.find("token") != std::string::npos || k.find("key") != std::string::npos || k.find("pass") != std::string::npos ||
                    k.find("sig") != std::string::npos || k.find("auth") != std::string::npos || k == "pw" || k == "pwd" || k.find("secret") != std::string::npos)
                    kv = kv.substr(0, eq + 1) + "***";
            }
            if (!out.empty()) out += "&";
            out += kv;
            if (j == tail.size()) break;
            i = j + 1;
        }
        r = head + "?" + out;
    }
    return r;
}

// ---------------- M3U ----------------
static std::vector<std::pair<std::string, std::string>> parseAttrs(const std::string& s, size_t& commaPos) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t i = 0;
    commaPos = std::string::npos;
    const size_t n = s.size();
    while (i < n) {
        while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i >= n) break;
        if (s[i] == ',') { commaPos = i; return out; }
        size_t k = i;
        while (k < n && s[k] != '=' && s[k] != ',' && s[k] != ' ' && s[k] != '\t') k++;
        if (k >= n || s[k] != '=') {
            // a bare word without '=': skip it (the comma may follow)
            if (k < n && s[k] == ',') { commaPos = k; return out; }
            i = k; continue;
        }
        std::string key = lower(s.substr(i, k - i));
        i = k + 1;
        std::string val;
        if (i < n && (s[i] == '"' || s[i] == '\'' || (unsigned char)s[i] == 0xE2)) {
            char q = s[i];
            if (q == '"' || q == '\'') {
                i++;
                size_t e = i;
                // a closing quote ends the value; some lists forget it: then the value ends at the next ` key=` or the comma
                while (e < n && s[e] != q) e++;
                val = s.substr(i, e - i);
                i = e < n ? e + 1 : n;
            }
        } else {
            size_t e = i;
            while (e < n && s[e] != ' ' && s[e] != '\t' && s[e] != ',') e++;
            val = s.substr(i, e - i);
            i = e;
        }
        if (!key.empty()) out.emplace_back(key, val);
    }
    return out;
}

std::string channelKey(const Channel& c) {
    if (!c.tvgId.empty()) return "id:" + c.tvgId;
    std::string u = c.url;
    const size_t q = u.find('?'); if (q != std::string::npos) u.resize(q);
    return "url:" + u;
}

static void setHeader(Channel& c, const std::string& k, const std::string& v) {
    const std::string lk = lower(k);
    if (lk == "user-agent") c.userAgent = v;
    else if (lk == "referer" || lk == "referrer") c.referer = v;
    else c.headers.emplace_back(k, v);
}

Playlist parseM3u(const std::string& textIn, const std::string& base) {
    Playlist pl;
    std::string text = textIn;
    if (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) text.erase(0, 3);
    if (text.size() >= 2 && (uint8_t)text[0] == 0xFF && (uint8_t)text[1] == 0xFE) return pl;   // UTF-16: not handled (the caller says so)
    if (text.find("#EXT-X-STREAM-INF") != std::string::npos || text.find("#EXT-X-TARGETDURATION") != std::string::npos ||
        text.find("#EXT-X-MEDIA-SEQUENCE") != std::string::npos) {
        // an HLS playlist, not a channel list: it is one stream
        Channel c; c.url = base; c.name = "Stream"; pl.channels.push_back(c); pl.isHlsStream = true;
        return pl;
    }
    pl.channels.reserve(1024);
    Channel cur; bool have = false;
    std::string pendGroup;
    std::set<std::string> seen;
    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        size_t e = i;
        while (e < n && text[e] != '\n' && text[e] != '\r') e++;
        std::string line = trim(text.substr(i, e - i));
        i = e + 1;
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (istarts(line, "#extm3u")) {
                size_t cp; auto at = parseAttrs(line.size() > 7 ? line.substr(7) : "", cp);
                for (auto& a : at) if ((a.first == "url-tvg" || a.first == "x-tvg-url" || a.first == "tvg-url") && pl.guideUrl.empty()) {
                    pl.guideUrl = a.second;
                    const size_t c = pl.guideUrl.find(',');   // several guides separated by commas: the first
                    if (c != std::string::npos) pl.guideUrl.resize(c);
                }
            } else if (istarts(line, "#extinf:")) {
                cur = Channel(); have = true;
                const std::string rest = line.substr(8);
                size_t cp;
                auto at = parseAttrs(rest, cp);
                // the duration is the first token (before any attribute)
                for (auto& a : at) {
                    if (a.first == "tvg-id") cur.tvgId = a.second;
                    else if (a.first == "tvg-name") cur.tvgName = a.second;
                    else if (a.first == "tvg-logo" || a.first == "logo" || a.first == "tvg-logo-small") { if (cur.logo.empty()) cur.logo = a.second; }
                    else if (a.first == "group-title") cur.group = a.second;
                    else if (a.first == "tvg-country") cur.country = a.second;
                    else if (a.first == "tvg-language") cur.language = a.second;
                    else if (a.first == "user-agent" || a.first == "http-user-agent") cur.userAgent = a.second;
                    else if (a.first == "referrer" || a.first == "referer" || a.first == "http-referrer") cur.referer = a.second;
                }
                if (cp != std::string::npos) cur.name = trim(rest.substr(cp + 1));
                if (cur.name.empty()) cur.name = cur.tvgName;
                if (cur.group.empty() && !pendGroup.empty()) cur.group = pendGroup;
            } else if (istarts(line, "#extgrp:")) {
                if (have && cur.group.empty()) cur.group = trim(line.substr(8)); else pendGroup = trim(line.substr(8));
            } else if (istarts(line, "#extvlcopt:")) {
                const std::string kv = line.substr(11);
                const size_t eq = kv.find('=');
                if (eq != std::string::npos && have) {
                    std::string k = lower(trim(kv.substr(0, eq))), v = trim(kv.substr(eq + 1));
                    if (k == "http-user-agent") cur.userAgent = v;
                    else if (k == "http-referrer" || k == "http-referer") cur.referer = v;
                    else if (startsWith(k, "http-")) setHeader(cur, k.substr(5), v);
                }
            } else if (istarts(line, "#exthttp:")) {
                // {"User-Agent":"x","Referer":"y"}: a flat JSON object of strings
                if (have) {
                    const std::string j = line.substr(9);
                    size_t p = 0;
                    while ((p = j.find('"', p)) != std::string::npos) {
                        size_t ke = j.find('"', p + 1); if (ke == std::string::npos) break;
                        std::string k = j.substr(p + 1, ke - p - 1);
                        size_t vs = j.find('"', j.find(':', ke) == std::string::npos ? ke : j.find(':', ke));
                        if (vs == std::string::npos) break;
                        size_t ve = j.find('"', vs + 1); if (ve == std::string::npos) break;
                        setHeader(cur, k, j.substr(vs + 1, ve - vs - 1));
                        p = ve + 1;
                    }
                }
            } else if (istarts(line, "#kodiprop:")) {
                const std::string l = lower(line);
                if (have && (l.find("license_type") != std::string::npos || l.find("license_key") != std::string::npos || l.find("widevine") != std::string::npos ||
                             l.find("playready") != std::string::npos || l.find("clearkey") != std::string::npos)) cur.drm = true;
            }
            continue;
        }
        // a URL line
        std::string url = line;
        const size_t pipe = url.find('|');   // Kodi style "url|User-Agent=x&Referer=y"
        std::string kodi;
        if (pipe != std::string::npos && looksLikeStreamUrl(url.substr(0, pipe))) { kodi = url.substr(pipe + 1); url.resize(pipe); }
        if (!have) { cur = Channel(); if (!pendGroup.empty()) cur.group = pendGroup; }
        cur.url = base.empty() ? url : resolveUrl(base, url);
        if (!kodi.empty()) {
            size_t p = 0;
            while (p <= kodi.size()) {
                size_t a = kodi.find('&', p); if (a == std::string::npos) a = kodi.size();
                const std::string kv = kodi.substr(p, a - p);
                const size_t eq = kv.find('=');
                if (eq != std::string::npos) setHeader(cur, kv.substr(0, eq), kv.substr(eq + 1));
                if (a == kodi.size()) break;
                p = a + 1;
            }
        }
        if (cur.name.empty()) {
            const Url u = parseUrl(cur.url);
            std::string nm = u.ok ? u.path : cur.url;
            const size_t q = nm.find('?'); if (q != std::string::npos) nm.resize(q);
            const size_t sl = nm.find_last_of('/');
            if (sl != std::string::npos && sl + 1 < nm.size()) nm = nm.substr(sl + 1);
            cur.name = nm.empty() || nm == "/" ? (u.ok ? u.host : cur.url) : nm;
        }
        if (looksLikeStreamUrl(cur.url) || !parseUrl(cur.url).ok) {
            const std::string key = cur.url + "\n" + cur.name;
            if (seen.insert(key).second) pl.channels.push_back(std::move(cur)); else pl.duplicatesDropped++;
        }
        cur = Channel(); have = false;
    }
    return pl;
}

// ---------------- XMLTV ----------------
static std::string xmlDecode(const std::string& s) {
    if (s.find('&') == std::string::npos) return s;
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') { o += s[i]; continue; }
        const size_t e = s.find(';', i);
        if (e == std::string::npos || e - i > 10) { o += s[i]; continue; }
        const std::string ent = s.substr(i + 1, e - i - 1);
        if (ent == "amp") o += '&'; else if (ent == "lt") o += '<'; else if (ent == "gt") o += '>'; else if (ent == "quot") o += '"'; else if (ent == "apos") o += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? strtoul(ent.c_str() + 2, nullptr, 16) : strtoul(ent.c_str() + 1, nullptr, 10);
            if (cp < 0x80) o += (char)cp;
            else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
            else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        } else { o += s.substr(i, e - i + 1); }
        i = e;
    }
    return o;
}

static int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

int64_t parseXmltvTime(const std::string& sIn) {
    const std::string s = trim(sIn);
    if (s.size() < 8) return 0;
    for (int i = 0; i < 8; i++) if (!std::isdigit((unsigned char)s[i])) return 0;
    auto num = [&](size_t p, size_t l) { int v = 0; for (size_t i = 0; i < l; i++) { if (p + i >= s.size() || !std::isdigit((unsigned char)s[p + i])) return -1; v = v * 10 + (s[p + i] - '0'); } return v; };
    const int Y = num(0, 4), M = num(4, 2), D = num(6, 2);
    int h = 0, mi = 0, se = 0;
    size_t p = 8;
    if (s.size() >= 10 && std::isdigit((unsigned char)s[8])) { h = num(8, 2); p = 10; }
    if (s.size() >= p + 2 && std::isdigit((unsigned char)s[p])) { mi = num(p, 2); p += 2; }
    if (s.size() >= p + 2 && std::isdigit((unsigned char)s[p])) { se = num(p, 2); p += 2; }
    if (Y < 1970 || M < 1 || M > 12 || D < 1 || D > 31 || h < 0 || h > 24 || mi < 0 || mi > 59 || se < 0 || se > 60) return 0;
    int64_t t = daysFromCivil(Y, (unsigned)M, (unsigned)D) * 86400 + h * 3600 + mi * 60 + se;
    while (p < s.size() && s[p] == ' ') p++;
    if (p < s.size() && (s[p] == '+' || s[p] == '-')) {
        const int sign = s[p] == '-' ? -1 : 1;
        p++;
        int oh = 0, om = 0;
        if (p + 2 <= s.size() && std::isdigit((unsigned char)s[p]) && std::isdigit((unsigned char)s[p + 1])) { oh = num(p, 2); p += 2; if (p < s.size() && s[p] == ':') p++; }
        if (p + 2 <= s.size() && std::isdigit((unsigned char)s[p]) && std::isdigit((unsigned char)s[p + 1])) om = num(p, 2);
        t -= sign * (oh * 3600 + om * 60);
    }
    return t;
}

static std::string attrOf(const std::string& tag, const char* name) {
    const std::string pat = std::string(name) + "=";
    size_t p = 0;
    while ((p = tag.find(pat, p)) != std::string::npos) {
        if (p > 0 && !std::isspace((unsigned char)tag[p - 1])) { p += pat.size(); continue; }
        size_t v = p + pat.size();
        if (v >= tag.size()) return "";
        const char q = tag[v];
        if (q != '"' && q != '\'') return "";
        const size_t e = tag.find(q, v + 1);
        if (e == std::string::npos) return "";
        return xmlDecode(tag.substr(v + 1, e - v - 1));
    }
    return "";
}

static std::string elemText(const std::string& body, const char* name) {
    const std::string open = std::string("<") + name;
    size_t p = 0;
    while ((p = body.find(open, p)) != std::string::npos) {
        const char c = p + open.size() < body.size() ? body[p + open.size()] : 0;
        if (c != '>' && c != ' ' && c != '/' && c != '\t' && c != '\n' && c != '\r') { p += open.size(); continue; }
        const size_t gt = body.find('>', p);
        if (gt == std::string::npos) return "";
        if (body[gt - 1] == '/') return "";
        const std::string close = std::string("</") + name + ">";
        const size_t e = body.find(close, gt);
        if (e == std::string::npos) return "";
        std::string t = body.substr(gt + 1, e - gt - 1);
        if (startsWith(t, "<![CDATA[")) { const size_t ce = t.find("]]>"); t = t.substr(9, ce == std::string::npos ? std::string::npos : ce - 9); }
        return trim(xmlDecode(t));
    }
    return "";
}

Guide parseXmltv(const std::string& xml) {
    Guide g;
    size_t p = 0, pgC = 0, chC = 0;   // cached next positions: searching again from p each time would be quadratic when one kind is exhausted
    bool pgDone = false, chDone = false, first = true;
    while (true) {
        if (!pgDone && (first || pgC < p)) { pgC = xml.find("<programme", p); if (pgC == std::string::npos) pgDone = true; }
        if (!chDone && (first || chC < p)) { chC = xml.find("<channel", p); if (chC == std::string::npos) chDone = true; }
        first = false;
        const size_t pg = pgDone ? std::string::npos : pgC;
        const size_t ch = chDone ? std::string::npos : chC;
        if (pg == std::string::npos && ch == std::string::npos) break;
        if (ch != std::string::npos && (pg == std::string::npos || ch < pg)) {
            const size_t c = ch + 8;
            if (c < xml.size() && !std::isspace((unsigned char)xml[c]) && xml[c] != '>') { p = c; continue; }   // <channels...
            const size_t gt = xml.find('>', ch);
            if (gt == std::string::npos) break;
            const std::string tag = xml.substr(ch, gt - ch);
            const std::string id = attrOf(tag, "id");
            size_t end = xml.find("</channel>", gt);
            if (end == std::string::npos) end = gt;
            if (!id.empty() && xml[gt - 1] != '/') { const std::string nm = elemText(xml.substr(gt, end - gt), "display-name"); if (!nm.empty()) g.names[id] = nm; }
            p = end;
            continue;
        }
        const size_t gt = xml.find('>', pg);
        if (gt == std::string::npos) break;
        const std::string tag = xml.substr(pg, gt - pg);
        const size_t end = xml.find("</programme>", gt);
        if (end == std::string::npos) break;
        Programme pr;
        pr.start = parseXmltvTime(attrOf(tag, "start"));
        pr.stop = parseXmltvTime(attrOf(tag, "stop"));
        const std::string chn = attrOf(tag, "channel");
        const std::string body = xml.substr(gt + 1, end - gt - 1);
        pr.title = elemText(body, "title");
        pr.desc = elemText(body, "desc");
        pr.category = elemText(body, "category");
        if (pr.start > 0 && !chn.empty()) {
            if (pr.stop <= pr.start) pr.stop = pr.start + 1800;
            g.byChannel[chn].push_back(std::move(pr));
            g.programmes++;
        }
        p = end + 12;
    }
    for (auto& kv : g.byChannel) std::stable_sort(kv.second.begin(), kv.second.end(), [](const Programme& a, const Programme& b) { return a.start < b.start; });
    return g;
}

const Programme* guideNow(const Guide& g, const Channel& c, int64_t now, const Programme** next) {
    if (next) *next = nullptr;
    const std::vector<Programme>* v = nullptr;
    auto find = [&](const std::string& id) { if (id.empty()) return; auto it = g.byChannel.find(id); if (it != g.byChannel.end() && !v) v = &it->second; };
    find(c.tvgId);
    if (!v) find(c.tvgName);
    if (!v) find(c.name);
    if (!v && !g.names.empty()) {
        const std::string ln = lower(c.name);
        for (auto& kv : g.names) if (lower(kv.second) == ln) { find(kv.first); break; }
    }
    if (!v) return nullptr;
    auto it = std::upper_bound(v->begin(), v->end(), now, [](int64_t t, const Programme& p) { return t < p.start; });
    const Programme* cur = nullptr;
    if (it != v->begin()) { const Programme* c2 = &*(it - 1); if (now < c2->stop) cur = c2; }
    if (next && it != v->end()) *next = &*it;
    return cur;
}

// ---------------- HLS ----------------
static std::map<std::string, std::string> hlsAttrs(const std::string& s) {
    std::map<std::string, std::string> m;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ',' || s[i] == ' ')) i++;
        size_t k = s.find('=', i);
        if (k == std::string::npos) break;
        std::string key = s.substr(i, k - i);
        i = k + 1;
        std::string val;
        if (i < s.size() && s[i] == '"') { size_t e = s.find('"', i + 1); if (e == std::string::npos) e = s.size(); val = s.substr(i + 1, e - i - 1); i = e + 1; }
        else { size_t e = s.find(',', i); if (e == std::string::npos) e = s.size(); val = s.substr(i, e - i); i = e; }
        m[key] = val;
    }
    return m;
}

HlsPlaylist parseHls(const std::string& textIn, const std::string& base) {
    HlsPlaylist pl;
    std::string text = textIn;
    if (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) text.erase(0, 3);
    size_t i = 0;
    while (i < text.size() && std::isspace((unsigned char)text[i])) i++;
    if (text.compare(i, 7, "#EXTM3U") != 0) { pl.error = "this is not an HLS playlist (no #EXTM3U)"; return pl; }
    pl.ok = true;
    HlsVariant pendingV; bool haveV = false;
    double pendDur = 0; bool pendDisc = false, haveInf = false;
    int64_t pendOff = -1, pendLen = 0, nextOff = 0;
    int64_t seq = 0; bool seqSet = false;
    std::string playlistType;
    while (i < text.size()) {
        size_t e = i;
        while (e < text.size() && text[e] != '\n' && text[e] != '\r') e++;
        std::string line = trim(text.substr(i, e - i));
        i = e + 1;
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (startsWith(line, "#EXT-X-STREAM-INF:")) {
                auto a = hlsAttrs(line.substr(18));
                pendingV = HlsVariant();
                pendingV.bandwidth = a.count("AVERAGE-BANDWIDTH") && atoll(a["AVERAGE-BANDWIDTH"].c_str()) > 0 && !a.count("BANDWIDTH") ? atoll(a["AVERAGE-BANDWIDTH"].c_str()) : atoll(a["BANDWIDTH"].c_str());
                if (a.count("RESOLUTION")) sscanf(a["RESOLUTION"].c_str(), "%dx%d", &pendingV.w, &pendingV.h);
                pendingV.codecs = a["CODECS"]; pendingV.audioGroup = a["AUDIO"]; pendingV.subGroup = a["SUBTITLES"];
                if (a.count("FRAME-RATE")) pendingV.fps = atof(a["FRAME-RATE"].c_str());
                haveV = true; pl.master = true;
            } else if (startsWith(line, "#EXT-X-MEDIA:")) {
                auto a = hlsAttrs(line.substr(13));
                HlsMedia m; m.type = a["TYPE"]; m.group = a["GROUP-ID"]; m.name = a["NAME"]; m.lang = a["LANGUAGE"];
                m.isDefault = a["DEFAULT"] == "YES";
                if (a.count("URI")) m.uri = resolveUrl(base, a["URI"]);
                pl.media.push_back(m); pl.master = true;
            } else if (startsWith(line, "#EXT-X-TARGETDURATION:")) pl.targetDuration = std::max(1.0, atof(line.c_str() + 22));
            else if (startsWith(line, "#EXT-X-MEDIA-SEQUENCE:")) { seq = atoll(line.c_str() + 22); seqSet = true; }
            else if (startsWith(line, "#EXT-X-ENDLIST")) pl.endList = true;
            else if (startsWith(line, "#EXT-X-PLAYLIST-TYPE:")) playlistType = trim(line.substr(21));
            else if (startsWith(line, "#EXTINF:")) { pendDur = atof(line.c_str() + 8); haveInf = true; }
            else if (startsWith(line, "#EXT-X-DISCONTINUITY") && !startsWith(line, "#EXT-X-DISCONTINUITY-SEQUENCE")) pendDisc = true;
            else if (startsWith(line, "#EXT-X-BYTERANGE:")) {
                const std::string r = line.substr(17);
                pendLen = atoll(r.c_str());
                const size_t at = r.find('@');
                pendOff = at != std::string::npos ? atoll(r.c_str() + at + 1) : nextOff;
                nextOff = pendOff + pendLen;
            } else if (startsWith(line, "#EXT-X-KEY:")) {
                auto a = hlsAttrs(line.substr(11));
                pl.keyMethod = a["METHOD"];
                pl.keyUri = a.count("URI") ? resolveUrl(base, a["URI"]) : "";
                if (a.count("KEYFORMAT") && a["KEYFORMAT"] != "identity") pl.keyMethod = "DRM:" + a["KEYFORMAT"];
            } else if (startsWith(line, "#EXT-X-MAP:")) pl.fmp4 = true;
            else if (startsWith(line, "#EXT-X-SESSION-KEY:")) {
                auto a = hlsAttrs(line.substr(19));
                if (a.count("KEYFORMAT") && a["KEYFORMAT"] != "identity") pl.keyMethod = "DRM:" + a["KEYFORMAT"];
            }
            continue;
        }
        if (haveV) { pendingV.uri = resolveUrl(base, line); pl.variants.push_back(pendingV); haveV = false; continue; }
        if (!haveInf && !pl.master) { /* a bare URI list: treat as segments of unknown length */ pendDur = 0; }
        HlsSegment s; s.uri = resolveUrl(base, line); s.dur = pendDur; s.seq = seq++; s.discontinuity = pendDisc; s.byteOff = pendOff; s.byteLen = pendLen;
        pl.segments.push_back(s);
        pendDisc = false; haveInf = false; pendOff = -1; pendLen = 0;
    }
    (void)seqSet;
    pl.mediaSequence = pl.segments.empty() ? seq : pl.segments.front().seq;
    pl.live = !pl.endList && playlistType != "VOD";
    if (pl.master && pl.variants.empty() && pl.segments.empty() && pl.media.empty()) { pl.ok = false; pl.error = "the playlist lists no streams"; }
    return pl;
}

size_t pickVariant(const std::vector<HlsVariant>& v, double thr, int forced, size_t cur) {
    if (v.empty()) return 0;
    if (forced >= 0) return std::min((size_t)forced, v.size() - 1);
    // v is sorted by bandwidth. Upgrade only with 30% head room, downgrade when the stream no longer fits.
    size_t best = 0;
    for (size_t i = 0; i < v.size(); i++) if ((double)v[i].bandwidth * 1.3 <= thr) best = i;
    if (thr <= 0) return std::min(cur, v.size() - 1);
    if (best > cur) return best;
    if ((double)v[std::min(cur, v.size() - 1)].bandwidth * 1.05 > thr) return best;
    return std::min(cur, v.size() - 1);
}

// ---------------- TS alignment and RTP ----------------
void TsAligner::feed(const uint8_t* d, size_t n) {
    buf_.insert(buf_.end(), d, d + n);
    size_t pos = 0;
    const size_t sz = buf_.size();
    size_t outStart = 0, outLen = 0;
    std::vector<uint8_t> out;
    out.reserve(sz);
    while (pos + 188 <= sz) {
        if (buf_[pos] == 0x47 && (pos + 188 == sz || (pos + 188 < sz && buf_[pos + 188] == 0x47))) {
            out.insert(out.end(), buf_.begin() + pos, buf_.begin() + pos + 188);
            pos += 188;
        } else if (buf_[pos] == 0x47 && pos + 188 >= sz) {
            break;
        } else {
            // find the next sync byte that is confirmed by another 0x47 188 bytes later
            size_t k = pos + 1;
            bool found = false;
            for (; k + 188 < sz; k++) if (buf_[k] == 0x47 && buf_[k + 188] == 0x47) { found = true; break; }
            if (!found) { // keep the last 188 bytes, drop the rest
                pos = sz > 188 ? sz - 188 : pos;
                if (pos > 0) resyncs_++;
                break;
            }
            resyncs_++;
            pos = k;
        }
    }
    (void)outStart; (void)outLen;
    buf_.erase(buf_.begin(), buf_.begin() + pos);
    if (!out.empty()) out_(out.data(), out.size());
}

bool rtpPayload(const uint8_t* d, size_t n, const uint8_t*& payload, size_t& len, uint16_t* seq) {
    if (n < 12 || (d[0] >> 6) != 2) return false;
    if (d[0] == 0x47) return false;
    const int cc = d[0] & 0x0F;
    size_t off = 12 + 4 * (size_t)cc;
    if (d[0] & 0x10) { if (off + 4 > n) return false; off += 4 + 4 * (size_t)(((d[off + 2] << 8) | d[off + 3])); }
    size_t end = n;
    if (d[0] & 0x20) { if (n == 0 || d[n - 1] > n - off) return false; end = n - d[n - 1]; }
    if (off >= end) return false;
    // payload type 33 is MP2T; dynamic types are accepted when the payload looks like transport stream
    const int pt = d[1] & 0x7F;
    if (pt != 33 && end - off >= 188 && d[off] != 0x47) return false;
    payload = d + off; len = end - off;
    if (seq) *seq = (uint16_t)((d[2] << 8) | d[3]);
    return true;
}

} // namespace dect2::iptv
