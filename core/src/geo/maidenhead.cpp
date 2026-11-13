// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/geo/maidenhead.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
// the deepest level (8 characters) is 1/120 degree of longitude and 1/240 of latitude: positions are counted in those units
long long units(double v, double off, double per, long long max) {
    long long u = (long long)std::floor((v + off) * per + 1e-9);
    return u < 0 ? 0 : u >= max ? max - 1 : u;
}
}

std::string toMaidenhead(double latDeg, double lonDeg, int chars) {
    if (!std::isfinite(latDeg) || !std::isfinite(lonDeg)) return "";
    chars = chars <= 2 ? 2 : chars <= 4 ? 4 : chars <= 6 ? 6 : 8;
    if (lonDeg < -180 || lonDeg > 180) { lonDeg = std::fmod(lonDeg + 180.0, 360.0); if (lonDeg < 0) lonDeg += 360; lonDeg -= 180; }
    const long long u = units(lonDeg, 180, 120, 360LL * 120), v = units(latDeg, 90, 240, 180LL * 240);
    std::string s;
    s += (char)('A' + u / 2400); s += (char)('A' + v / 2400);
    if (chars >= 4) { s += (char)('0' + (u / 240) % 10); s += (char)('0' + (v / 240) % 10); }
    if (chars >= 6) { s += (char)('a' + (u / 10) % 24); s += (char)('a' + (v / 10) % 24); }
    if (chars >= 8) { s += (char)('0' + u % 10); s += (char)('0' + v % 10); }
    return s;
}

bool fromMaidenhead(const std::string& in, double& latDeg, double& lonDeg, double* heightDeg, double* widthDeg) {
    std::string l;
    for (char c : in) if (!std::isspace((unsigned char)c)) l += (char)std::toupper((unsigned char)c);
    if (l.size() != 2 && l.size() != 4 && l.size() != 6 && l.size() != 8) return false;
    double lon = -180, lat = -90, w = 20, h = 10;
    for (size_t i = 0; i < l.size(); i += 2) {
        const char a = l[i], b = l[i + 1];
        const int lvl = (int)i / 2;
        if (lvl == 0) { if (a < 'A' || a > 'R' || b < 'A' || b > 'R') return false; }
        else if (lvl == 2) { if (a < 'A' || a > 'X' || b < 'A' || b > 'X') return false; }
        else if (a < '0' || a > '9' || b < '0' || b > '9') return false;
        const int x = lvl % 2 ? a - '0' : a - 'A', y = lvl % 2 ? b - '0' : b - 'A';
        if (lvl > 0) { const double d = lvl == 2 ? 24 : 10; w /= d; h /= d; }
        lon += x * w; lat += y * h;
    }
    lonDeg = lon + w / 2; latDeg = lat + h / 2;
    if (heightDeg) *heightDeg = h;
    if (widthDeg) *widthDeg = w;
    return true;
}

bool parseCoordinates(const std::string& text, double& latDeg, double& lonDeg) {
    struct Group { std::vector<double> n; char hemi = 0; bool neg = false; };
    std::vector<Group> g(1);
    auto close = [&] { if (!g.back().n.empty()) g.emplace_back(); };
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = (unsigned char)text[i];
        if (std::isdigit(c) || c == '.' || ((c == '-' || c == '+') && i + 1 < text.size() && (std::isdigit((unsigned char)text[i + 1]) || text[i + 1] == '.'))) {
            char* e = nullptr;
            const double v = std::strtod(text.c_str() + i, &e);
            if (e == text.c_str() + i) return false;
            if (g.back().n.size() == 3) close();   // a fourth number starts the longitude
            if (g.back().n.empty() && (c == '-')) g.back().neg = true;
            else if (c == '-') return false;       // a minus sign on minutes or seconds
            g.back().n.push_back(std::fabs(v));
            i = (size_t)(e - text.c_str());
            continue;
        }
        const char u = (char)std::toupper(c);
        if (u == 'N' || u == 'S' || u == 'E' || u == 'W') {
            if (!g.back().n.empty() && !g.back().hemi) { g.back().hemi = u; close(); }   // 54.7N
            else { close(); if (g.back().hemi) return false; g.back().hemi = u; }        // N54.7
        } else if (c == ',' || c == ';') close();
        else if (std::isalpha(c)) return false;
        i++;   // spaces, degree, minute and second signs (any byte of a UTF-8 sign) separate numbers
    }
    if (g.back().n.empty() && !g.back().hemi) g.pop_back();
    if (g.size() == 1 && !g[0].hemi && (g[0].n.size() == 2 || g[0].n.size() == 4 || g[0].n.size() == 6)) {   // "54 41 13 25 16 48"
        Group b; const size_t k = g[0].n.size() / 2;
        b.n.assign(g[0].n.begin() + k, g[0].n.end()); g[0].n.resize(k);
        g.push_back(b);
    }
    if (g.size() != 2) return false;
    double v[2];
    for (int j = 0; j < 2; j++) {
        const Group& q = g[j];
        if (q.n.empty() || q.n.size() > 3) return false;
        for (size_t k = 1; k < q.n.size(); k++) if (q.n[k] >= 60 || q.n[k - 1] != std::floor(q.n[k - 1])) return false;   // only the last part has decimals
        v[j] = q.n[0] + (q.n.size() > 1 ? q.n[1] / 60 : 0) + (q.n.size() > 2 ? q.n[2] / 3600 : 0);
        if (q.neg) v[j] = -v[j];
        if (q.hemi == 'S' || q.hemi == 'W') { if (q.neg) return false; v[j] = -v[j]; }
    }
    const bool lonFirst = g[0].hemi == 'E' || g[0].hemi == 'W' || g[1].hemi == 'N' || g[1].hemi == 'S';
    if (lonFirst && ((g[0].hemi == 'N' || g[0].hemi == 'S') || (g[1].hemi == 'E' || g[1].hemi == 'W'))) return false;   // two latitudes
    latDeg = lonFirst ? v[1] : v[0]; lonDeg = lonFirst ? v[0] : v[1];
    return std::fabs(latDeg) <= 90 && std::fabs(lonDeg) <= 180;
}

std::string toDms(double deg, bool isLat) {
    const char h = isLat ? (deg < 0 ? 'S' : 'N') : (deg < 0 ? 'W' : 'E');
    double a = std::fabs(deg);
    int d = (int)a; a = (a - d) * 60; int m = (int)a; double s = (a - m) * 60;
    if (s >= 59.95) { s = 0; if (++m == 60) { m = 0; d++; } }
    char b[48];
    snprintf(b, sizeof b, "%d\xC2\xB0%02d'%04.1f\"%c", d, m, s, h);
    return b;
}

void greatCircle(double lat1, double lon1, double lat2, double lon2, double& km, double& bearingDeg) {
    const double r = kPi / 180, p1 = lat1 * r, p2 = lat2 * r, dl = (lon2 - lon1) * r;
    const double a = std::sin((p2 - p1) / 2) * std::sin((p2 - p1) / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    km = 2 * 6371.0088 * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1 - a)));
    const double b = std::atan2(std::sin(dl) * std::cos(p2), std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl)) / r;
    bearingDeg = std::fmod(b + 360, 360);
}

}
