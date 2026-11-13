// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Contact graph for QSO-style modes: message parser, callsign prefix positions, great-circle arcs (contact_graph.h).
#include "dect2/map/contact_graph.h"
#include "dect2/geo/maidenhead.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace dect2 {
namespace {

struct Pfx { const char* p; double lat, lon; };
const Pfx kPfx[] = {
#include "prefix_table.inc"
};

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> w;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { if (!cur.empty()) { w.push_back(cur); cur.clear(); } }
        else cur += (char)std::toupper((unsigned char)c);
    }
    if (!cur.empty()) w.push_back(cur);
    return w;
}

bool isGridText(const std::string& s) {   // AA00 .. RR99 (RR73 is the word, not a locator)
    if (s.size() != 4 && s.size() != 6) return false;
    if (s == "RR73") return false;
    if (s[0] < 'A' || s[0] > 'R' || s[1] < 'A' || s[1] > 'R' || !std::isdigit((unsigned char)s[2]) || !std::isdigit((unsigned char)s[3])) return false;
    if (s.size() == 6 && (s[4] < 'A' || s[4] > 'X' || s[5] < 'A' || s[5] > 'X')) return false;
    return true;
}

bool isCallText(const std::string& s) {
    if (s.size() < 3 || s.size() > 13) return false;
    bool dig = false, let = false;
    for (char c : s) {
        if (std::isdigit((unsigned char)c)) dig = true;
        else if (std::isalpha((unsigned char)c)) let = true;
        else if (c != '/') return false;
    }
    return dig && let && s.front() != '/' && s.back() != '/';
}

bool isReport(const std::string& s) {   // -12, +05, R-05, R+10
    size_t i = (!s.empty() && s[0] == 'R') ? 1 : 0;
    if (i >= s.size() || (s[i] != '+' && s[i] != '-')) return false;
    for (size_t k = i + 1; k < s.size(); k++) if (!std::isdigit((unsigned char)s[k])) return false;
    return s.size() > i + 1;
}

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit((unsigned char)c)) return false;
    return true;
}

// "<K1ABC>" -> K1ABC, "<...>" -> "" (a hashed call that could not be resolved); ok=false when the word is not a call
bool callWord(std::string w, std::string& out, bool& hashed) {
    hashed = false;
    out.clear();
    if (w.size() >= 2 && w.front() == '<' && w.back() == '>') {
        w = w.substr(1, w.size() - 2);
        if (w.empty() || w.find_first_not_of('.') == std::string::npos) { hashed = true; return true; }
    }
    if (!isCallText(w)) return false;
    out = w;
    return true;
}

bool isCqTag(const std::string& w) {   // DX, POTA, NA, EU, SOTA, 000-999
    if (w.empty() || w.size() > 4) return false;
    if (allDigits(w)) return w.size() <= 3;
    for (char c : w) if (!std::isalpha((unsigned char)c)) return false;
    return true;
}

void payload(ContactMsg& m, const std::vector<std::string>& w, size_t from) {
    for (size_t i = from; i < w.size(); i++) {
        const std::string& t = w[i];
        if (t == "RR73") { m.complete = true; m.final73 = true; }
        else if (t == "RRR") m.complete = true;
        else if (t == "73") { m.complete = true; m.final73 = true; }
        else if (isGridText(t)) m.grid = t.substr(0, 4);
        else if (isReport(t) && m.report.empty()) m.report = t;
    }
}

constexpr double kPi = 3.14159265358979323846;

}

std::string baseCallsign(const std::string& call) {
    std::string up;
    for (char c : call) up += (char)std::toupper((unsigned char)c);
    std::vector<std::string> parts;
    std::string cur;
    for (char c : up) { if (c == '/') { parts.push_back(cur); cur.clear(); } else cur += c; }
    parts.push_back(cur);
    std::string best;
    for (const auto& p : parts) {
        bool dig = false;
        for (char c : p) dig |= std::isdigit((unsigned char)c) != 0;
        if (dig && p.size() > best.size()) best = p;
    }
    return best.empty() ? up : best;
}

ContactMsg parseContactMessage(const std::string& msg) {
    ContactMsg m;
    std::vector<std::string> w = words(msg);
    if (w.size() < 2) return m;
    bool hashed = false;
    std::string c1, c2;
    // JS8Call's display form: "LY2ABC: OK1XX SNR -12", "LY2ABC: @HB HEARTBEAT KO24", "LY2ABC: CQ CQ CQ KO24"
    if (w[0].size() > 3 && w[0].back() == ':') {
        std::string from;
        if (!callWord(w[0].substr(0, w[0].size() - 1), from, hashed) || from.empty()) return m;
        m.from = from;
        if (w[1] == "CQ" || w[1] == "@HB") {
            m.cq = true;
            m.valid = true;
            payload(m, w, 2);
            return m;
        }
        if (callWord(w[1], c2, hashed) && !c2.empty()) { m.to = c2; m.valid = true; payload(m, w, 2); return m; }
        return m;   // free text or a group (@ALLCALL): no contact
    }
    const bool cq = w[0] == "CQ" || w[0] == "QRZ";
    if (cq) {
        size_t i = 1;
        while (i + 1 < w.size() && isCqTag(w[i])) { if (m.cqTag.empty()) m.cqTag = w[i]; i++; }
        if (i >= w.size() || !callWord(w[i], c1, hashed)) return m;
        m.cq = true;
        m.from = c1;
        m.valid = !c1.empty();
        payload(m, w, i + 1);
        return m;
    }
    if (!callWord(w[0], c1, hashed)) return m;
    // WSPR: "CALL GRID DBM"
    if (w.size() == 3 && isGridText(w[1]) && allDigits(w[2])) {
        m.from = c1;
        m.grid = w[1].substr(0, 4);
        m.cq = true;   // heard calling nobody in particular: a position only
        m.valid = !c1.empty();
        return m;
    }
    if (!callWord(w[1], c2, hashed)) return m;
    m.to = c1;
    m.from = c2;
    m.valid = !c1.empty() || !c2.empty();
    payload(m, w, 2);
    return m;
}

bool callsignPrefixPosition(const std::string& call, double& lat, double& lon) {
    std::string up;
    for (char c : call) { if (c == '/') break; up += (char)std::toupper((unsigned char)c); }
    if (up.size() < 1) return false;
    // countries with many prefixes map to one table entry: United States (K N W AA-AL), England (G M 2E), Russia (R U), Germany (DA-DR), China (B)
    const char a = up[0], b = up.size() > 1 ? up[1] : ' ';
    std::string key;
    if (a == 'N' || a == 'W' || (a == 'A' && b >= 'A' && b <= 'L')) key = "K";
    else if (a == 'M' || (a == '2' && b == 'E')) key = "G";
    else if (a == 'U' && b >= 'A' && b <= 'I') key = "R";
    else if (a == 'D' && b >= 'A' && b <= 'R') key = "DL";
    else if (a == 'B') key = "BY";
    // longest prefix of the table
    const Pfx* best = nullptr;
    size_t bestLen = 0;
    for (const Pfx& p : kPfx) {
        const size_t n = std::strlen(p.p);
        if (n > up.size() || n <= bestLen) continue;
        if (up.compare(0, n, p.p) == 0) { best = &p; bestLen = n; }
    }
    if (!key.empty() && !best) {
        for (const Pfx& p : kPfx) if (key == p.p) best = &p;
    }
    if (!best) return false;
    lat = best->lat;
    lon = best->lon;
    return true;
}

std::vector<std::pair<double, double>> greatCircleArc(double lat1, double lon1, double lat2, double lon2, double stepDeg) {
    std::vector<std::pair<double, double>> out;
    const double r = kPi / 180;
    auto vec = [&](double la, double lo, double v[3]) { v[0] = std::cos(la * r) * std::cos(lo * r); v[1] = std::cos(la * r) * std::sin(lo * r); v[2] = std::sin(la * r); };
    double a[3], b[3];
    vec(lat1, lon1, a);
    vec(lat2, lon2, b);
    const double dot = std::max(-1.0, std::min(1.0, a[0] * b[0] + a[1] * b[1] + a[2] * b[2]));
    const double ang = std::acos(dot);
    const double deg = ang / r;
    if (stepDeg < 0.1) stepDeg = 0.1;
    int n = (int)std::ceil(deg / stepDeg);
    n = std::max(1, std::min(399, n));
    out.reserve(n + 1);
    out.push_back({lat1, lon1});
    double prev = lon1;
    for (int i = 1; i < n; i++) {
        const double f = (double)i / n;
        double p[3];
        if (ang < 1e-9) { p[0] = a[0]; p[1] = a[1]; p[2] = a[2]; }
        else {
            const double s = std::sin(ang), k1 = std::sin((1 - f) * ang) / s, k2 = std::sin(f * ang) / s;
            for (int j = 0; j < 3; j++) p[j] = k1 * a[j] + k2 * b[j];
        }
        const double la = std::atan2(p[2], std::sqrt(p[0] * p[0] + p[1] * p[1])) / r;
        double lo = std::atan2(p[1], p[0]) / r;
        while (lo - prev > 180) lo -= 360;
        while (lo - prev < -180) lo += 360;
        out.push_back({la, lo});
        prev = lo;
    }
    double e = lon2;
    while (e - prev > 180) e -= 360;
    while (e - prev < -180) e += 360;
    out.push_back({lat2, e});
    return out;
}

ContactStation& ContactGraph::touch(const std::string& call, double t) {
    ContactStation& s = stations_[call];
    if (s.call.empty()) {
        s.call = call;
        double la, lo;
        if (callsignPrefixPosition(call, la, lo)) { s.lat = la; s.lon = lo; s.known = true; s.estimated = true; }
    }
    s.lastSeen = std::max(s.lastSeen, t);
    return s;
}

bool ContactGraph::add(double t, const std::string& msg, int mode, double dialHz, const std::string& senderGrid) {
    const ContactMsg m = parseContactMessage(msg);
    if (!m.valid) return false;
    const std::string from = m.from.empty() ? std::string() : baseCallsign(m.from);
    const std::string to = m.to.empty() ? std::string() : baseCallsign(m.to);
    if (!from.empty()) {
        ContactStation& s = touch(from, t);
        const std::string& g = !m.grid.empty() ? m.grid : senderGrid;
        double la, lo;
        if (!g.empty() && fromMaidenhead(g, la, lo)) { s.lat = la; s.lon = lo; s.known = true; s.estimated = false; s.grid = g.substr(0, 4); }
    }
    if (!to.empty()) touch(to, t);
    if (from.empty() || to.empty() || from == to || m.cq) return true;
    const auto key = from < to ? std::make_pair(from, to) : std::make_pair(to, from);
    ContactEdge& e = edges_[key];
    const bool fresh = e.count == 0;
    // a new exchange (a locator or a plain report, no closing word) a while after a finished one starts the contact again
    if (!fresh && e.complete && !m.complete && (!m.grid.empty() || (!m.report.empty() && m.report[0] != 'R')) && t - e.last > 60) { e.complete = false; e.first = t; e.count = 0; }
    if (e.count == 0) e.first = t;
    e.from = from;
    e.to = to;
    e.last = std::max(e.last, t);
    e.count++;
    e.mode = mode;
    e.dialHz = dialHz;
    e.lastMsg = msg;
    if (m.complete) e.complete = true;
    return true;
}

void ContactGraph::expire(double now) {
    const double cut = now - expiry_;
    for (auto it = edges_.begin(); it != edges_.end();) it = it->second.last < cut ? edges_.erase(it) : std::next(it);
    std::map<std::string, bool> used;
    for (const auto& e : edges_) { used[e.second.from] = true; used[e.second.to] = true; }
    for (auto it = stations_.begin(); it != stations_.end();) it = (it->second.lastSeen < cut && !used.count(it->first)) ? stations_.erase(it) : std::next(it);
}

std::vector<ContactStation> ContactGraph::stations() const {
    std::vector<ContactStation> v;
    for (const auto& s : stations_) v.push_back(s.second);
    return v;
}

std::vector<ContactEdge> ContactGraph::edges() const {
    std::vector<ContactEdge> v;
    for (const auto& e : edges_) v.push_back(e.second);
    std::sort(v.begin(), v.end(), [](const ContactEdge& a, const ContactEdge& b) { return a.last < b.last; });
    return v;
}

bool ContactGraph::station(const std::string& call, ContactStation& out) const {
    const auto it = stations_.find(baseCallsign(call));
    if (it == stations_.end()) return false;
    out = it->second;
    return true;
}

}
