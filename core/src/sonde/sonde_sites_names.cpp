// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Known radiosonde launch sites: the lookups and the user's file (sonde_sites_known.h); the table itself is sonde_sites_known.cpp.
#include "dect2/sonde/sonde_sites_known.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace dect2 {

double sondeSiteKm(double la1, double lo1, double la2, double lo2) {
    const double r = 3.14159265358979 / 180, a = std::sin((la2 - la1) * r / 2), b = std::sin((lo2 - lo1) * r / 2);
    const double h = a * a + std::cos(la1 * r) * std::cos(la2 * r) * b * b;
    return 6371.0 * 2 * std::atan2(std::sqrt(h), std::sqrt(std::max(0.0, 1 - h)));
}

KnownSondeSites::KnownSondeSites() {
    size_t n = 0;
    const KnownSondeSite* t = builtinSondeSites(n);
    all_.reserve(n);
    for (size_t i = 0; i < n; i++) { SondeSiteRef r; r.name = t[i].name; r.lat = t[i].lat; r.lon = t[i].lon; r.altM = t[i].altM; r.wmo = t[i].wmo; all_.push_back(r); }
    builtin_ = n;
}

int KnownSondeSites::addUserText(const std::string& text) {
    std::stringstream ss(text);
    std::string l;
    int n = 0;
    while (std::getline(ss, l)) {
        const size_t h = l.find('#');
        if (h != std::string::npos) l.resize(h);
        for (char& c : l) if (c == '\t' || c == ',') c = ';';
        std::vector<std::string> f;
        std::stringstream ls(l);
        std::string x;
        while (std::getline(ls, x, ';')) {
            while (!x.empty() && (x.back() == ' ' || x.back() == '\r')) x.pop_back();
            size_t i = 0; while (i < x.size() && x[i] == ' ') i++;
            f.push_back(x.substr(i));
        }
        if (f.size() < 3 || f[0].empty()) continue;
        char* e1 = nullptr; char* e2 = nullptr;
        const double la = std::strtod(f[1].c_str(), &e1), lo = std::strtod(f[2].c_str(), &e2);
        if (e1 == f[1].c_str() || e2 == f[2].c_str() || !(std::fabs(la) <= 90) || !(std::fabs(lo) <= 180)) continue;
        SondeSiteRef r; r.name = f[0]; r.lat = la; r.lon = lo; r.user = true;
        if (f.size() > 3) r.altM = std::atof(f[3].c_str());
        all_.push_back(r);
        n++;
    }
    return n;
}

const SondeSiteRef* KnownSondeSites::nearest(double lat, double lon, double maxKm, double* km) const {
    const SondeSiteRef* best = nullptr; double bd = 1e18;
    const SondeSiteRef* bestUser = nullptr; double bu = 1e18;
    for (const SondeSiteRef& s : all_) {
        if (std::fabs(s.lat - lat) > maxKm / 100.0 + 0.2) continue;   // quick reject: a degree of latitude is 111 km
        const double d = sondeSiteKm(lat, lon, s.lat, s.lon);
        if (d > maxKm) continue;
        if (s.user && d < bu) { bu = d; bestUser = &s; }
        if (d < bd) { bd = d; best = &s; }
    }
    if (bestUser) { if (km) *km = bu; return bestUser; }
    if (best && km) *km = bd;
    return best;
}

std::vector<const SondeSiteRef*> KnownSondeSites::inBox(double south, double west, double north, double east, size_t maxN) const {
    auto inside = [&](const SondeSiteRef& s) {
        if (s.lat < south || s.lat > north) return false;
        return west <= east ? (s.lon >= west && s.lon <= east) : (s.lon >= west || s.lon <= east);
    };
    std::vector<const SondeSiteRef*> user, built;
    for (size_t i = builtin_; i < all_.size(); i++) if (inside(all_[i])) user.push_back(&all_[i]);
    for (size_t i = 0; i < builtin_; i++) {
        const SondeSiteRef& s = all_[i];
        if (!inside(s)) continue;
        bool shadowed = false;
        for (const SondeSiteRef* u : user) if (sondeSiteKm(s.lat, s.lon, u->lat, u->lon) < 2) { shadowed = true; break; }
        if (!shadowed) built.push_back(&s);
    }
    std::vector<const SondeSiteRef*> out = user;
    if (out.size() >= maxN) { out.resize(maxN); return out; }
    const size_t room = maxN - out.size();
    if (built.size() <= room) out.insert(out.end(), built.begin(), built.end());
    else for (size_t k = 0; k < room; k++) out.push_back(built[k * built.size() / room]);   // zoomed far out: an even spread
    return out;
}

}  // namespace dect2
