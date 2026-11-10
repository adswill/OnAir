// plane-alert-db import: see plane_alert.h.
#include "dect2/watchlist/plane_alert.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dect2::watchlist {

namespace {
std::vector<std::string> csvRow(const std::string& l) {
    std::vector<std::string> v;
    std::string cur;
    bool q = false;
    for (size_t i = 0; i < l.size(); i++) {
        const char c = l[i];
        if (q) { if (c == '"') { if (i + 1 < l.size() && l[i + 1] == '"') { cur += '"'; i++; } else q = false; } else cur += c; }
        else if (c == '"') q = true;
        else if (c == ',') { v.push_back(cur); cur.clear(); }
        else cur += c;
    }
    v.push_back(cur);
    return v;
}
std::string colName(std::string s) {
    std::string o;
    for (char c : s) if (c != '$' && c != '#' && c != '\r' && c != '\xEF' && c != '\xBB' && c != '\xBF') o += (char)std::tolower((unsigned char)c);
    while (!o.empty() && o.front() == ' ') o.erase(o.begin());
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}
std::string trim(const std::string& s) { size_t a = 0, b = s.size(); while (a < b && std::isspace((unsigned char)s[a])) a++; while (b > a && std::isspace((unsigned char)s[b - 1])) b--; return s.substr(a, b - a); }
}  // namespace

std::string PlaneAlertEntry::tagText() const {
    std::string o;
    for (const auto& t : tags) if (!t.empty()) o += (o.empty() ? "" : ", ") + t;
    return o;
}

bool parsePlaneAlert(const std::string& csv, std::vector<PlaneAlertEntry>& out, std::string& err) {
    std::stringstream ss(csv);
    std::string l;
    if (!std::getline(ss, l)) { err = "the file is empty"; return false; }
    const auto hdr = csvRow(l);
    std::map<std::string, size_t> col;
    for (size_t i = 0; i < hdr.size(); i++) col[colName(hdr[i])] = i;
    auto at = [&](const std::vector<std::string>& r, const char* name) { auto it = col.find(name); return it == col.end() || it->second >= r.size() ? std::string() : trim(r[it->second]); };
    if (!col.count("icao")) { err = "no ICAO column: not a plane-alert-db file"; return false; }
    std::vector<PlaneAlertEntry> v;
    while (std::getline(ss, l)) {
        if (l.empty() || l == "\r") continue;
        const auto r = csvRow(l);
        const std::string hex = at(r, "icao");
        if (hex.size() != 6) continue;
        char* e = nullptr;
        const unsigned long x = std::strtoul(hex.c_str(), &e, 16);
        if (!e || *e) continue;
        PlaneAlertEntry p;
        p.icao = (uint32_t)x;
        p.registration = at(r, "registration"); p.operatorName = at(r, "operator"); p.type = at(r, "type"); p.icaoType = at(r, "icao type");
        p.cmpg = at(r, "cmpg"); p.category = at(r, "category");
        for (const char* t : {"tag 1", "tag 2", "tag 3"}) { const std::string s = at(r, t); if (!s.empty()) p.tags.push_back(s); }
        v.push_back(p);
    }
    if (v.empty()) { err = "no aircraft in the file"; return false; }
    out = std::move(v);
    return true;
}

std::vector<List> planeAlertLists(const std::vector<PlaneAlertEntry>& v) {
    std::map<std::string, std::vector<std::string>> byCat;
    for (const auto& p : v) { char b[8]; std::snprintf(b, sizeof b, "%06X", p.icao); byCat[p.category.empty() ? "Uncategorised" : p.category].push_back(b); }
    std::vector<List> out;
    int n = 1;
    for (auto& kv : byCat) {
        List l;
        l.id = "pa-" + std::to_string(n++);
        l.name = "plane-alert: " + kv.first;
        l.kind = Kind::Aircraft;
        l.enabled = false;   // the user switches on the categories wanted
        std::sort(kv.second.begin(), kv.second.end());
        kv.second.erase(std::unique(kv.second.begin(), kv.second.end()), kv.second.end());
        l.values = std::move(kv.second);
        out.push_back(std::move(l));
    }
    return out;
}

bool isPlaneAlertList(const List& l) { return l.id.rfind("pa-", 0) == 0 && l.name.rfind("plane-alert: ", 0) == 0; }

std::map<uint32_t, PlaneAlertEntry> planeAlertIndex(const std::vector<PlaneAlertEntry>& v) { std::map<uint32_t, PlaneAlertEntry> m; for (const auto& p : v) m[p.icao] = p; return m; }

}  // namespace dect2::watchlist
