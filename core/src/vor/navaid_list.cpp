// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The near-me list of navigation aids (dect2/vor/navaid_list.h).
#include "dect2/vor/navaid_list.h"
#include "dect2/vor/vor_db.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

namespace dect2 {

namespace {

// One CSV line with quoted fields ("a, b" and doubled quotes)
std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    bool q = false;
    for (size_t i = 0; i < s.size(); i++) {
        const char c = s[i];
        if (q) {
            if (c == '"') { if (i + 1 < s.size() && s[i + 1] == '"') { cur += '"'; i++; } else q = false; }
            else cur += c;
        } else if (c == '"') q = true;
        else if (c == ',') { v.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    v.push_back(cur);
    return v;
}

unsigned kindOf(const std::string& type) {
    if (type == "VOR" || type == "VOR-DME" || type == "VORTAC" || type == "DVOR" || type == "DVOR-DME") return NavaidVor;
    if (type == "NDB" || type == "NDB-DME") return NavaidNdb;
    if (type == "DME" || type == "TACAN") return NavaidDme;
    return 0;
}

}  // namespace

int navaidParseCsv(const std::string& text, std::vector<Navaid>& out, std::string& err) {
    err.clear();
    size_t pos = 0;
    int read = 0;
    std::map<std::string, int> col;
    bool header = true;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        if (line.empty()) continue;
        const std::vector<std::string> f = splitCsv(line);
        if (header) {
            header = false;
            for (int i = 0; i < (int)f.size(); i++) col[f[(size_t)i]] = i;
            for (const char* need : {"ident", "type", "frequency_khz", "latitude_deg", "longitude_deg"})
                if (!col.count(need)) { err = std::string("not a navaids.csv: no column ") + need; return 0; }
            continue;
        }
        auto get = [&](const char* name) -> std::string { auto it = col.find(name); return it != col.end() && it->second < (int)f.size() ? f[(size_t)it->second] : std::string(); };
        Navaid n;
        n.ident = get("ident"); n.name = get("name"); n.type = get("type"); n.country = get("iso_country");
        n.kind = kindOf(n.type);
        if (!n.kind || n.ident.empty()) continue;
        const std::string khz = get("frequency_khz"), dme = get("dme_frequency_khz");
        double fr = std::atof(khz.c_str()) / 1000.0;
        if (n.kind == NavaidDme && !dme.empty()) fr = std::atof(dme.c_str()) / 1000.0;
        n.freqMhz = fr;
        const std::string la = get("latitude_deg"), lo = get("longitude_deg");
        if (la.empty() || lo.empty() || !(fr > 0)) continue;
        n.lat = std::atof(la.c_str()); n.lon = std::atof(lo.c_str());
        if (!(n.lat >= -90 && n.lat <= 90 && n.lon >= -180 && n.lon <= 180)) continue;
        out.push_back(std::move(n));
        read++;
    }
    if (read == 0) err = "no stations in the file";
    return read;
}

std::vector<NavaidNear> navaidNearest(const std::vector<Navaid>& db, double lat, double lon, unsigned kindMask, double maxKm, size_t n) {
    std::vector<NavaidNear> v;
    for (size_t i = 0; i < db.size(); i++) {
        if (!(db[i].kind & kindMask)) continue;
        const double km = navDistanceM(lat, lon, db[i].lat, db[i].lon) / 1000.0;
        if (km > maxKm) continue;
        v.push_back({i, km, navBearingDeg(lat, lon, db[i].lat, db[i].lon)});
    }
    std::sort(v.begin(), v.end(), [](const NavaidNear& a, const NavaidNear& b) { return a.km < b.km; });
    if (v.size() > n) v.resize(n);
    return v;
}

bool navaidBestCrossing(const std::vector<NavaidNear>& near, int& a, int& b, double* crossDeg) {
    double best = -1;
    for (int i = 0; i < (int)near.size(); i++)
        for (int j = i + 1; j < (int)near.size(); j++) {
            double d = std::fmod(std::fabs(near[(size_t)i].bearingDeg - near[(size_t)j].bearingDeg), 180.0);
            if (d > 90) d = 180 - d;   // radials cross at the acute or obtuse angle: 90 is the best fix
            if (d > best) { best = d; a = i; b = j; if (crossDeg) *crossDeg = d; }
        }
    return best >= 0;
}

}  // namespace dect2
