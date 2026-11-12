// Callsign lookup for the QSO panel (4.0): see ham_callbook.h.
#include "dect2/ham/ham_callbook.h"
#include "dect2/common/xmlmini.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace dect2::ham {

namespace {
std::string upper(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
std::string clean(const std::string& s) {   // one line, no tabs: what the cache file can hold
    std::string o;
    for (char c : s) o += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    return trim(o);
}
std::string urlenc(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}
std::string child(const std::shared_ptr<XmlNode>& n, const char* name) {
    if (!n) return "";
    const auto c = n->first(name);
    return c ? trim(c->text) : "";
}
std::shared_ptr<XmlNode> findDeep(const std::shared_ptr<XmlNode>& n, const std::string& name) {
    if (!n) return nullptr;
    if (n->local() == name) return n;
    for (const auto& c : n->children) if (auto r = findDeep(c, name)) return r;
    return nullptr;
}
bool yes(const std::string& v) { const std::string u = upper(v); return u == "1" || u == "Y" || u == "YES"; }
int num(const std::string& v) { return std::atoi(v.c_str()); }
}  // namespace

CallbookReply parseQrzXml(const std::string& xml) {
    CallbookReply r;
    const auto root = parseXml(xml);
    if (!root) { r.error = "the answer is not XML"; return r; }
    const auto sess = findDeep(root, "Session");
    r.sessionKey = child(sess, "Key");
    r.error = child(sess, "Error");
    const auto c = findDeep(root, "Callsign");
    if (!c) { if (r.error.empty() && r.sessionKey.empty()) r.error = "no callsign in the answer"; return r; }
    CallbookEntry& e = r.e;
    e.call = upper(child(c, "call"));
    const std::string fn = child(c, "fname"), ln = child(c, "name"), nick = child(c, "nickname");
    e.name = trim(fn + (fn.empty() || ln.empty() ? "" : " ") + ln);
    if (e.name.empty()) e.name = nick;
    e.qth = child(c, "addr2");
    e.state = upper(child(c, "state"));
    e.county = child(c, "county");
    e.country = child(c, "country");
    e.grid = child(c, "grid");
    e.qslVia = child(c, "qslmgr");
    const std::string la = child(c, "lat"), lo = child(c, "lon");
    if (!la.empty() && !lo.empty()) { e.lat = std::atof(la.c_str()); e.lon = std::atof(lo.c_str()); e.hasPos = true; }
    e.dxcc = num(child(c, "dxcc"));
    e.cqz = num(child(c, "cqzone"));
    e.ituz = num(child(c, "ituzone"));
    e.lotw = yes(child(c, "lotw"));
    e.eqsl = yes(child(c, "eqsl"));
    e.source = "QRZ";
    r.ok = !e.call.empty();
    return r;
}

CallbookReply parseHamQthXml(const std::string& xml) {
    CallbookReply r;
    const auto root = parseXml(xml);
    if (!root) { r.error = "the answer is not XML"; return r; }
    const auto sess = findDeep(root, "session");
    r.sessionKey = child(sess, "session_id");
    r.error = child(sess, "error");
    const auto s = findDeep(root, "search");
    if (!s) { if (r.error.empty() && r.sessionKey.empty()) r.error = "no callsign in the answer"; return r; }
    CallbookEntry& e = r.e;
    e.call = upper(child(s, "callsign"));
    e.name = child(s, "nick");
    if (e.name.empty()) e.name = child(s, "adr_name");
    e.qth = child(s, "qth");
    if (e.qth.empty()) e.qth = child(s, "adr_city");
    e.state = upper(child(s, "us_state"));
    e.county = child(s, "us_county");
    e.country = child(s, "country");
    e.grid = child(s, "grid");
    e.qslVia = child(s, "qsl_via");
    const std::string la = child(s, "latitude"), lo = child(s, "longitude");
    if (!la.empty() && !lo.empty()) { e.lat = std::atof(la.c_str()); e.lon = std::atof(lo.c_str()); e.hasPos = true; }
    e.dxcc = num(child(s, "adif"));
    e.cqz = num(child(s, "cq"));
    e.ituz = num(child(s, "itu"));
    e.lotw = yes(child(s, "lotw"));
    e.eqsl = yes(child(s, "eqsl"));
    e.source = "HamQTH";
    r.ok = !e.call.empty();
    return r;
}

CallbookEntry callbookFromLog(const std::vector<LogQso>& log, const std::string& call) {
    CallbookEntry e;
    const std::string b = baseCall(upper(call));
    if (b.empty()) return e;
    // newest first: the most recent value of each field wins
    std::vector<const LogQso*> v;
    for (const auto& q : log) if (baseCall(q.call) == b) v.push_back(&q);
    std::sort(v.begin(), v.end(), [](const LogQso* x, const LogQso* y) { return x->startUtc > y->startUtc; });
    for (const LogQso* q : v) {
        if (e.name.empty()) e.name = q->name;
        if (e.qth.empty()) e.qth = q->qth;
        if (e.state.empty()) e.state = q->state;
        if (e.country.empty()) e.country = q->country;
        if (e.grid.empty() || (e.grid.size() < 6 && q->grid.size() >= 6 && q->grid.compare(0, 4, e.grid.substr(0, 4)) == 0)) { if (!q->grid.empty()) e.grid = q->grid; }
        if (!e.dxcc) e.dxcc = q->dxcc;
        if (!e.cqz) e.cqz = q->cqz;
        if (!e.ituz) e.ituz = q->ituz;
        const auto c = q->extra.find("CNTY");
        if (e.county.empty() && c != q->extra.end()) e.county = c->second;
    }
    if (!v.empty()) { e.call = b; e.source = "log"; }
    return e;
}

int callbookFill(LogQso& q, const CallbookEntry& e) {
    int n = 0;
    auto fill = [&n](std::string& dst, const std::string& src) { if (dst.empty() && !src.empty()) { dst = src; n++; } };
    fill(q.name, e.name);
    fill(q.qth, e.qth);
    fill(q.state, e.state);
    fill(q.country, e.country);
    if (q.grid.empty() && !e.grid.empty()) { q.grid = upper(e.grid); n++; }
    if (!q.dxcc && e.dxcc) { q.dxcc = e.dxcc; n++; }
    if (!q.cqz && e.cqz) { q.cqz = e.cqz; n++; }
    if (!q.ituz && e.ituz) { q.ituz = e.ituz; n++; }
    if (!e.county.empty() && !q.extra.count("CNTY")) { q.extra["CNTY"] = e.county; n++; }
    return n;
}

void callbookMerge(CallbookEntry& e, const CallbookEntry& m) {
    auto f = [](std::string& d, const std::string& s) { if (d.empty()) d = s; };
    f(e.call, m.call); f(e.name, m.name); f(e.qth, m.qth); f(e.state, m.state); f(e.county, m.county); f(e.country, m.country);
    f(e.qslVia, m.qslVia); f(e.source, m.source);
    if (e.grid.empty() || (e.grid.size() < m.grid.size() && m.grid.compare(0, e.grid.size(), e.grid) == 0)) { if (!m.grid.empty()) e.grid = m.grid; }
    if (!e.hasPos && m.hasPos) { e.lat = m.lat; e.lon = m.lon; e.hasPos = true; }
    if (!e.dxcc) e.dxcc = m.dxcc;
    if (!e.cqz) e.cqz = m.cqz;
    if (!e.ituz) e.ituz = m.ituz;
    e.lotw = e.lotw || m.lotw;
    e.eqsl = e.eqsl || m.eqsl;
    if (!e.fetchedUtc) e.fetchedUtc = m.fetchedUtc;
}

// call name qth state county country grid qslVia lat lon dxcc cqz ituz lotw eqsl source fetched
std::string callbookCacheLine(const CallbookEntry& e) {
    char tail[160];
    snprintf(tail, sizeof tail, "%s\t%s\t%d\t%d\t%d\t%d\t%d\t", e.hasPos ? std::to_string(e.lat).c_str() : "", e.hasPos ? std::to_string(e.lon).c_str() : "",
             e.dxcc, e.cqz, e.ituz, e.lotw ? 1 : 0, e.eqsl ? 1 : 0);
    char t[32];
    snprintf(t, sizeof t, "%.0f", e.fetchedUtc);
    return clean(upper(e.call)) + "\t" + clean(e.name) + "\t" + clean(e.qth) + "\t" + clean(e.state) + "\t" + clean(e.county) + "\t" + clean(e.country) +
           "\t" + clean(e.grid) + "\t" + clean(e.qslVia) + "\t" + tail + clean(e.source) + "\t" + t;
}
bool callbookCacheParse(const std::string& line, CallbookEntry& e) {
    std::vector<std::string> f;
    std::string cur;
    for (char c : line) { if (c == '\t') { f.push_back(cur); cur.clear(); } else if (c != '\r' && c != '\n') cur += c; }
    f.push_back(cur);
    if (f.size() < 17 || f[0].empty()) return false;
    e = CallbookEntry();
    e.call = upper(f[0]); e.name = f[1]; e.qth = f[2]; e.state = f[3]; e.county = f[4]; e.country = f[5]; e.grid = f[6]; e.qslVia = f[7];
    if (!f[8].empty() && !f[9].empty()) { e.lat = std::atof(f[8].c_str()); e.lon = std::atof(f[9].c_str()); e.hasPos = true; }
    e.dxcc = num(f[10]); e.cqz = num(f[11]); e.ituz = num(f[12]); e.lotw = f[13] == "1"; e.eqsl = f[14] == "1"; e.source = f[15];
    e.fetchedUtc = std::atof(f[16].c_str());
    return true;
}

CallbookRequest callbookLoginRequest(const CallbookConfig& c) {
    CallbookRequest r;
    if (c.service == CallbookService::Qrz) {
        r.url = "https://xmldata.qrz.com/xml/current/";
        r.body = "username=" + urlenc(c.user) + "&password=" + urlenc(c.password) + "&agent=OnAir";
    } else if (c.service == CallbookService::HamQth) {
        r.url = "https://www.hamqth.com/xml.php";
        r.body = "u=" + urlenc(c.user) + "&p=" + urlenc(c.password);
    }
    return r;
}
CallbookRequest callbookLookupRequest(const CallbookConfig& c, const std::string& session, const std::string& call) {
    CallbookRequest r;
    if (c.service == CallbookService::Qrz) {
        r.url = "https://xmldata.qrz.com/xml/current/";
        r.body = "s=" + urlenc(session) + "&callsign=" + urlenc(upper(call));
    } else if (c.service == CallbookService::HamQth) {
        r.url = "https://www.hamqth.com/xml.php";
        r.body = "id=" + urlenc(session) + "&callsign=" + urlenc(upper(call)) + "&prg=OnAir";
    }
    return r;
}

void CallbookClient::setConfig(const CallbookConfig& c) {
    std::lock_guard<std::mutex> l(mu_);
    if (c.service != cfg_.service || c.user != cfg_.user || c.password != cfg_.password) session_.clear();
    cfg_ = c;
}
CallbookConfig CallbookClient::config() const { std::lock_guard<std::mutex> l(mu_); return cfg_; }
void CallbookClient::forgetSession() { std::lock_guard<std::mutex> l(mu_); session_.clear(); }
size_t CallbookClient::cacheSize() const { std::lock_guard<std::mutex> l(mu_); return cache_.size(); }

bool CallbookClient::loadCache(const std::string& path) {
    std::lock_guard<std::mutex> l(mu_);
    path_ = path;
    cache_.clear();
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        CallbookEntry e;
        if (callbookCacheParse(line, e)) cache_[e.call] = e;
    }
    return true;
}
bool CallbookClient::store(const CallbookEntry& e) {   // under mu_
    if (e.call.empty()) return false;
    cache_[upper(e.call)] = e;
    if (path_.empty()) return true;
    {
        std::ofstream o(path_ + ".tmp", std::ios::trunc);
        for (const auto& kv : cache_) o << callbookCacheLine(kv.second) << '\n';
        o.flush();
        if (!o) return false;
    }
    std::remove(path_.c_str());
    return std::rename((path_ + ".tmp").c_str(), path_.c_str()) == 0;
}

CallbookEntry CallbookClient::local(const std::string& call, const std::vector<LogQso>* log) const {
    CallbookEntry e;
    {
        std::lock_guard<std::mutex> l(mu_);
        const auto it = cache_.find(upper(call));
        if (it != cache_.end()) { e = it->second; e.source = "cache"; }
        else {
            const auto ib = cache_.find(baseCall(upper(call)));
            if (ib != cache_.end()) { e = ib->second; e.source = "cache"; }
        }
    }
    if (log) {
        const CallbookEntry fl = callbookFromLog(*log, call);
        if (!fl.empty()) { if (e.empty()) e = fl; else callbookMerge(e, fl); }
    }
    return e;
}

bool CallbookClient::lookup(const std::string& call0, double now, CallbookEntry& out, std::string& err) {
    const std::string call = upper(trim(call0));
    if (call.empty()) { err = "no callsign"; return false; }
    CallbookConfig c;
    std::string session;
    {
        std::lock_guard<std::mutex> l(mu_);
        const auto it = cache_.find(call);
        if (it != cache_.end() && (!cfg_.ready() || now - it->second.fetchedUtc < cfg_.maxAgeDays * 86400.0)) { out = it->second; out.source = "cache"; return true; }
        if (!cfg_.ready()) { err = "the online lookup is off"; return false; }
        c = cfg_;
        session = session_;
    }
    auto parse = [&](const std::string& body) { return c.service == CallbookService::Qrz ? parseQrzXml(body) : parseHamQthXml(body); };
    auto login = [&]() {
        const CallbookRequest rq = callbookLoginRequest(c);
        const UpResponse rs = tr_(rq.url, rq.body);
        { std::lock_guard<std::mutex> l(mu_); requests_++; }
        if (rs.status < 200 || rs.status >= 300) { err = rs.err.empty() ? "login failed (HTTP " + std::to_string(rs.status) + ")" : rs.err; return false; }
        const CallbookReply r = parse(rs.body);
        if (r.sessionKey.empty()) { err = r.error.empty() ? "login failed" : r.error; return false; }
        session = r.sessionKey;
        std::lock_guard<std::mutex> l(mu_);
        session_ = session;
        return true;
    };
    for (int attempt = 0; attempt < 2; attempt++) {
        if (session.empty() && !login()) return false;
        const CallbookRequest rq = callbookLookupRequest(c, session, call);
        const UpResponse rs = tr_(rq.url, rq.body);
        { std::lock_guard<std::mutex> l(mu_); requests_++; }
        if (rs.status < 200 || rs.status >= 300) { err = rs.err.empty() ? "lookup failed (HTTP " + std::to_string(rs.status) + ")" : rs.err; return false; }
        const CallbookReply r = parse(rs.body);
        if (r.ok) {
            out = r.e;
            out.fetchedUtc = now;
            std::lock_guard<std::mutex> l(mu_);
            store(out);
            return true;
        }
        const std::string el = upper(r.error);
        const bool expired = el.find("SESSION") != std::string::npos || el.find("EXPIRED") != std::string::npos || el.find("INVALID") != std::string::npos ||
                             (r.sessionKey.empty() && el.find("NOT FOUND") == std::string::npos);
        if (expired && attempt == 0) {
            session.clear();
            std::lock_guard<std::mutex> l(mu_);
            session_.clear();
            continue;
        }
        err = r.error.empty() ? "not found" : r.error;
        return false;
    }
    err = "lookup failed";
    return false;
}

}  // namespace dect2::ham
