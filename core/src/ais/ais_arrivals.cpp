// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AIS arrivals board: see ais_arrivals.h.
#include "dect2/ais/ais_arrivals.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>

namespace dect2 {

namespace {
size_t lev(const std::string& a, const std::string& b) {
    std::vector<size_t> p(b.size() + 1), c(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) p[j] = j;
    for (size_t i = 1; i <= a.size(); i++) {
        c[0] = i;
        for (size_t j = 1; j <= b.size(); j++) c[j] = std::min({p[j] + 1, c[j - 1] + 1, p[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(p, c);
    }
    return p[b.size()];
}
int64_t civil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
double hav(double la1, double lo1, double la2, double lo2) {
    const double r = 3.14159265358979 / 180, a = std::sin((la2 - la1) * r / 2), b = std::sin((lo2 - lo1) * r / 2);
    const double h = a * a + std::cos(la1 * r) * std::cos(la2 * r) * b * b;
    return 6371.0 * 2 * std::atan2(std::sqrt(h), std::sqrt(1 - h));
}
}  // namespace

std::string aisDestinationNorm(const std::string& s0) {
    std::string s = s0;
    const size_t g = s.rfind('>');
    if (g != std::string::npos) s = s.substr(g + 1);
    std::string o;
    for (unsigned char c : s) if (std::isalnum(c)) o += (char)std::toupper(c);
    return o;
}

bool aisDestinationMatches(const std::string& dest, const AisPort& port) {
    const std::string d = aisDestinationNorm(dest);
    if (d.empty()) return false;
    std::string lc = aisDestinationNorm(port.locode);
    if (lc.size() == 5 && (d == lc || (d.size() > 5 && d.compare(0, 5, lc) == 0 && !std::isalpha((unsigned char)d[5])))) return true;   // "LTKLJ", "LTKLJ01"
    std::vector<std::string> names = port.aliases;
    names.push_back(port.name);
    for (const std::string& n0 : names) {
        const std::string n = aisDestinationNorm(n0);
        if (n.size() < 3) continue;
        if (d == n) return true;
        if (d.size() > n.size() && d.compare(0, n.size(), n) == 0) return true;            // "KLAIPEDAROADS", "KLAIPEDAANCH"
        if (d.size() == n.size() + 2 && d.compare(2, std::string::npos, n) == 0) return true;   // a country prefix: "LTKLAIPEDA"
        if (n.size() >= 6 && lev(d, n) <= 1) return true;                                 // "KLAIPEDS", "KLAPEDA"
        if (n.size() >= 6 && d.size() > n.size() && lev(d.substr(0, n.size()), n) <= 1) return true;
    }
    return false;
}

int64_t aisEtaUtc(int month, int day, int hour, int minute, int64_t now) {
    if (month < 1 || month > 12 || day < 1 || day > 31) return 0;
    if (hour < 0 || hour > 23) hour = 0;
    if (minute < 0 || minute > 59) minute = 0;
    const time_t tt = (time_t)now;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    for (int y = g.tm_year + 1900 - 1; y <= g.tm_year + 1900 + 1; y++) {
        const int64_t t = civil(y, month, day) * 86400 + hour * 3600 + minute * 60;
        if (t >= now - 86400) return t;
    }
    return 0;
}

std::vector<AisArrival> aisArrivals(const std::vector<AisVessel>& vs, const AisPort& port, int64_t now) {
    std::vector<AisArrival> out;
    for (const AisVessel& v : vs) {
        if (v.destination.empty() || !aisDestinationMatches(v.destination, port)) continue;
        AisArrival a;
        a.mmsi = v.mmsi; a.name = v.name; a.destination = v.destination; a.sog = v.sog; a.draughtM = v.draughtM;
        a.type = v.shipType >= 0 ? aisShipTypeText(v.shipType) : "";
        a.etaDeclared = aisEtaUtc(v.etaMonth, v.etaDay, v.etaHour, v.etaMin, now);
        if (v.hasPos && port.hasPos) {
            a.distKm = hav(v.lat, v.lon, port.lat, port.lon);
            if (v.sog >= 0.5f) a.etaWorked = now + (int64_t)std::llround(a.distKm / (v.sog * 1.852) * 3600);
        }
        out.push_back(a);
    }
    std::sort(out.begin(), out.end(), [](const AisArrival& x, const AisArrival& y) {
        const int64_t a = x.etaDeclared ? x.etaDeclared : (x.etaWorked ? x.etaWorked + (int64_t)1e12 : (int64_t)4e12);
        const int64_t b = y.etaDeclared ? y.etaDeclared : (y.etaWorked ? y.etaWorked + (int64_t)1e12 : (int64_t)4e12);
        return a != b ? a < b : x.mmsi < y.mmsi;
    });
    return out;
}

std::string aisPortToText(const AisPort& p) {
    auto c = [](std::string s) { for (char& ch : s) if (ch == '|' || ch == ';' || ch == '\n') ch = ' '; return s; };
    char b[64];
    std::snprintf(b, sizeof b, "%.6f|%.6f", p.lat, p.lon);
    std::string al;
    for (const auto& a : p.aliases) { if (!al.empty()) al += ";"; al += c(a); }
    return c(p.name) + "|" + c(p.locode) + "|" + (p.hasPos ? std::string(b) : std::string("|")) + "|" + al;
}

AisPort aisPortFromText(const std::string& s) {
    AisPort p;
    std::vector<std::string> f;
    std::stringstream ss(s);
    std::string x;
    while (std::getline(ss, x, '|')) f.push_back(x);
    if (f.size() >= 1) p.name = f[0];
    if (f.size() >= 2) p.locode = f[1];
    if (f.size() >= 4 && !f[2].empty() && !f[3].empty()) { p.lat = std::atof(f[2].c_str()); p.lon = std::atof(f[3].c_str()); p.hasPos = std::fabs(p.lat) <= 90 && std::fabs(p.lon) <= 180; }
    if (f.size() >= 5) { std::stringstream as(f[4]); while (std::getline(as, x, ';')) if (!x.empty()) p.aliases.push_back(x); }
    return p;
}

}  // namespace dect2
