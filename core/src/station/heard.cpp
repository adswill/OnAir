// Search everything heard: see heard.h.
#include "dect2/station/heard.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <unordered_set>

namespace dect2::station {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

bool allDigits(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); }); }
bool allHex(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c); }); }

bool isCallsign(const std::string& s) {
    std::string base = s;
    const size_t dash = s.find('-');
    if (dash != std::string::npos) {
        const std::string ssid = s.substr(dash + 1);
        if (ssid.empty() || ssid.size() > 2 || !allDigits(ssid)) return false;
        base = s.substr(0, dash);
    }
    if (base.size() < 3 || base.size() > 8) return false;
    bool alpha = false, digit = false;
    for (unsigned char c : base) {
        if (std::isalpha(c)) alpha = true;
        else if (std::isdigit(c)) digit = true;
        else return false;
    }
    return alpha && digit;
}

// number followed by an optional unit; multiplier 0 = no unit
bool parseNumberUnit(const std::string& s, double& value, double& mult, bool& hasPoint) {
    const char* b = s.c_str();
    char* e = nullptr;
    value = std::strtod(b, &e);
    if (e == b || value < 0) return false;
    hasPoint = std::string(b, (size_t)(e - b)).find('.') != std::string::npos;
    std::string u = lower(trim(std::string(e)));
    if (u.empty()) mult = 0;
    else if (u == "hz") mult = 1;
    else if (u == "k" || u == "khz") mult = 1e3;
    else if (u == "m" || u == "mhz") mult = 1e6;
    else if (u == "g" || u == "ghz") mult = 1e9;
    else return false;
    return true;
}

bool isFreqStart(const std::string& s) { return !s.empty() && (std::isdigit((unsigned char)s[0]) || s[0] == '.'); }

} // namespace

Query parseQuery(const std::string& in, double tolHz) {
    Query q;
    q.text = trim(in);
    if (q.text.empty()) return q;
    // a range of MHz: "144-146", "144 - 146 MHz". An -SSID call sign does not get here: it would have to start with two plain numbers.
    if (isFreqStart(q.text)) {
        const size_t dash = q.text.find('-');
        if (dash != std::string::npos && dash > 0) {
            double a = 0, b = 0, ma = 0, mb = 0;
            bool pa = false, pb = false;
            if (parseNumberUnit(trim(q.text.substr(0, dash)), a, ma, pa) && parseNumberUnit(trim(q.text.substr(dash + 1)), b, mb, pb) && b > a && a > 0) {
                const double m = mb > 0 ? mb : ma > 0 ? ma : 1e6;
                q.kind = QueryKind::Frequency; q.loHz = a * (ma > 0 ? ma : m); q.hiHz = b * m;
                return q;
            }
        }
        double v = 0, m = 0;
        bool point = false;
        if (parseNumberUnit(q.text, v, m, point) && v > 0 && (m > 0 || point)) {
            const double hz = v * (m > 0 ? m : 1e6);
            q.kind = QueryKind::Frequency; q.loHz = hz - tolHz; q.hiHz = hz + tolHz;
            return q;
        }
    }
    q.kind = identKind(q.text);
    return q;
}

QueryKind identKind(const std::string& s) {
    if (s.empty()) return QueryKind::Empty;
    if (s.size() == 9 && allDigits(s)) return QueryKind::Mmsi;
    if (s.size() == 6 && allHex(s)) return QueryKind::Icao;
    if (isCallsign(s)) return QueryKind::Callsign;
    return QueryKind::Text;
}

std::vector<Heard> searchHeard(const msgdb::Db& db, const std::string& query, const LiveLookup& live, int maxRows) {
    std::vector<Heard> out;
    const Query q = parseQuery(query);
    if (q.kind == QueryKind::Empty) return out;
    std::vector<msgdb::Record> rows;
    if (q.kind == QueryKind::Frequency) {
        msgdb::Filter f; f.freqLo = q.loHz; f.freqHi = q.hiHz; f.limit = maxRows;
        rows = db.query(f);
    } else {
        msgdb::Filter f; f.ident = q.text; f.limit = maxRows;
        rows = db.query(f);
        msgdb::Filter t; t.text = q.text; t.limit = maxRows;
        std::vector<msgdb::Record> more = db.query(t);
        std::unordered_set<int64_t> seen;
        for (const msgdb::Record& r : rows) seen.insert(r.id);
        for (msgdb::Record& r : more) if (seen.insert(r.id).second) rows.push_back(std::move(r));
    }
    struct Group { Heard h; double posT = -1; double textT = -1; std::set<std::string> modes; };
    std::map<std::string, Group> groups;
    for (const msgdb::Record& r : rows) {
        const std::string key = r.ident.empty() ? "\x01" + r.mode : lower(r.ident);
        Group& g = groups[key];
        if (g.h.count == 0) { g.h.ident = r.ident; g.h.kind = r.ident.empty() ? QueryKind::Text : identKind(r.ident); g.h.first = g.h.last = r.time; }
        g.h.count++;
        g.modes.insert(r.mode);
        g.h.first = std::min(g.h.first, r.time);
        g.h.last = std::max(g.h.last, r.time);
        if (r.time >= g.textT) { g.textT = r.time; g.h.lastText = r.text; if (r.freqHz > 0) g.h.lastFreqHz = r.freqHz; }
        if (r.hasPos && r.time >= g.posT) { g.posT = r.time; g.h.hasPos = true; g.h.lat = r.lat; g.h.lon = r.lon; }
    }
    const std::string wanted = lower(q.text);
    for (auto& kv : groups) {
        Heard& h = kv.second.h;
        h.modes.assign(kv.second.modes.begin(), kv.second.modes.end());
        double la = 0, lo = 0;
        if (live && !h.ident.empty() && live(h.ident, la, lo)) { h.hasPos = true; h.lat = la; h.lon = lo; h.live = true; }
        out.push_back(std::move(h));
    }
    std::sort(out.begin(), out.end(), [&](const Heard& a, const Heard& b) {
        const bool ea = lower(a.ident) == wanted, eb = lower(b.ident) == wanted;
        if (ea != eb) return ea;
        return a.last > b.last;
    });
    return out;
}

} // namespace dect2::station
