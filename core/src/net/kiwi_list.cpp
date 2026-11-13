// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The list of public KiwiSDR receivers (parseKiwiList, kiwisdr.h): a JSON array of flat objects, as the public listing publishes it, also when it is
// wrapped in a JavaScript assignment. A tolerant reader: unknown fields and nested values are skipped, a bad entry is dropped, garbage gives {}.
// The field names are ASSUMED from the public listing (name, url, loc, antenna, sdr_hw, gps, users, users_max, snr, bands, offline): the endpoint
// could not be reached while this was written (see docs/kiwisdr.md).
#include "dect2/net/kiwisdr.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

namespace dect2 {

namespace {
using Obj = std::map<std::string, std::string>;

struct Rd {
    const std::string& s;
    size_t i = 0;
    long budget = 8000000;   // steps: a hostile input cannot make it run long
    explicit Rd(const std::string& t) : s(t) {}
    bool more() const { return i < s.size() && budget > 0; }
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++; }
    bool str(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        out.clear();
        while (i < s.size() && s[i] != '"') {
            budget--;
            char c = s[i++];
            if (c == '\\' && i < s.size()) {
                const char e = s[i++];
                if (e == 'n') c = '\n';
                else if (e == 't') c = '\t';
                else if (e == 'u') {
                    if (i + 4 > s.size()) return false;
                    const unsigned v = (unsigned)strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                    i += 4;
                    if (v < 0x80) c = (char)v;
                    else if (v < 0x800) { out += (char)(0xC0 | v >> 6); c = (char)(0x80 | (v & 63)); }
                    else { out += (char)(0xE0 | v >> 12); out += (char)(0x80 | (v >> 6 & 63)); c = (char)(0x80 | (v & 63)); }
                } else c = e;
            }
            out += c;
            if (out.size() > 2000) return false;
        }
        if (i >= s.size()) return false;
        i++;
        return true;
    }
    // skips one bracketed value (balanced, at most 16 deep)
    bool skipBracketed() {
        int depth = 0;
        bool in = false;
        for (; i < s.size() && budget-- > 0; i++) {
            const char c = s[i];
            if (in) { if (c == '\\') i++; else if (c == '"') in = false; }
            else if (c == '"') in = true;
            else if (c == '{' || c == '[') { if (++depth > 16) return false; }
            else if (c == '}' || c == ']') { if (--depth == 0) { i++; return true; } }
        }
        return false;
    }
    bool object(Obj& o) {
        o.clear();
        ws();
        if (i >= s.size() || s[i] != '{') return false;
        i++;
        ws();
        if (i < s.size() && s[i] == '}') { i++; return true; }
        while (more()) {
            ws();
            std::string k, v;
            if (!str(k)) return false;
            ws();
            if (i >= s.size() || s[i] != ':') return false;
            i++;
            ws();
            if (i >= s.size()) return false;
            if (s[i] == '"') { if (!str(v)) return false; }
            else if (s[i] == '{' || s[i] == '[') { if (!skipBracketed()) return false; }
            else {
                const size_t b = i;
                while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ' ' && s[i] != '\n' && s[i] != '\r' && s[i] != '\t') i++;
                v = s.substr(b, std::min<size_t>(i - b, 64));
            }
            if (o.size() < 128) o[k] = v;
            ws();
            if (i >= s.size()) return false;
            if (s[i] == ',') { i++; continue; }
            if (s[i] == '}') { i++; return true; }
            return false;
        }
        return false;
    }
};

std::string get(const Obj& o, const char* k) { auto it = o.find(k); return it == o.end() ? std::string() : it->second; }
bool num(const std::string& s, double& v) {
    const char* b = s.c_str();
    while (*b == ' ' || *b == '(') b++;
    char* e = nullptr;
    v = strtod(b, &e);
    return e != b && std::isfinite(v);
}

bool toServer(const Obj& o, KiwiServer& k) {
    k = KiwiServer();
    std::string url = get(o, "url");
    for (const char* pre : {"http://", "https://", "ws://", "wss://"}) if (url.rfind(pre, 0) == 0) { url.erase(0, std::string(pre).size()); break; }
    const size_t sl = url.find('/');
    if (sl != std::string::npos) url.erase(sl);
    int port = 8073;
    const size_t br = url.rfind(']'), c = url.rfind(':');
    if (c != std::string::npos && (br == std::string::npos || c > br)) {
        const int p = atoi(url.c_str() + c + 1);
        if (p > 0 && p < 65536) port = p;
        url.erase(c);
    }
    if (url.size() > 2 && url.front() == '[' && url.back() == ']') url = url.substr(1, url.size() - 2);
    if (url.empty() || url.size() > 253) return false;
    for (char ch : url) if ((unsigned char)ch <= ' ' || ch == ',' || ch == ';') return false;
    k.host = url;
    k.port = port;
    k.name = get(o, "name"); k.location = get(o, "loc"); k.antenna = get(o, "antenna"); k.hardware = get(o, "sdr_hw"); k.id = get(o, "id");
    double v;
    if (num(get(o, "users"), v) && v >= 0 && v < 1000) k.users = (int)v;
    if (num(get(o, "users_max"), v) && v >= 0 && v < 1000) k.usersMax = (int)v;
    const std::string off = get(o, "offline");
    k.offline = off == "yes" || off == "true" || off == "1";
    if (num(get(o, "snr"), v) && v > 0 && v < 100) k.snr = v;   // "30,25": the 0-30 MHz figure comes first
    const std::string g = get(o, "gps");                          // "(51.5, -0.12)"
    const size_t cm = g.find(',');
    double la, lo;
    if (cm != std::string::npos && num(g.substr(0, cm), la) && num(g.substr(cm + 1), lo) && std::fabs(la) <= 90 && std::fabs(lo) <= 180 && !(la == 0 && lo == 0)) {
        k.hasPosition = true; k.lat = la; k.lon = lo;
    }
    const std::string bands = get(o, "bands");   // "0-30000000"
    const size_t d = bands.find('-', 1);
    double b0, b1;
    if (d != std::string::npos && num(bands.substr(0, d), b0) && num(bands.substr(d + 1), b1) && b1 > b0) { k.loHz = b0; k.hiHz = b1; }
    return true;
}
}  // namespace

std::vector<KiwiServer> parseKiwiList(const std::string& text) {
    std::vector<KiwiServer> out;
    const size_t b = text.find('[');
    if (b == std::string::npos) return out;
    Rd r(text);
    r.i = b + 1;
    Obj o;
    while (r.more() && out.size() < 5000) {
        r.ws();
        if (r.i >= text.size() || text[r.i] == ']') break;
        if (text[r.i] == ',') { r.i++; continue; }
        if (text[r.i] != '{') break;
        if (!r.object(o)) break;
        KiwiServer k;
        if (toServer(o, k)) out.push_back(std::move(k));
    }
    return out;
}

double greatCircleKm(double la1, double lo1, double la2, double lo2) {
    const double r = 3.14159265358979323846 / 180, R = 6371.0;
    const double dla = (la2 - la1) * r, dlo = (lo2 - lo1) * r;
    const double a = std::sin(dla / 2) * std::sin(dla / 2) + std::cos(la1 * r) * std::cos(la2 * r) * std::sin(dlo / 2) * std::sin(dlo / 2);
    return 2 * R * std::asin(std::min(1.0, std::sqrt(a)));
}

void kiwiSortByDistance(std::vector<KiwiServer>& v, double lat, double lon) {
    for (auto& k : v) k.distanceKm = k.hasPosition ? greatCircleKm(lat, lon, k.lat, k.lon) : -1;
    std::stable_sort(v.begin(), v.end(), [](const KiwiServer& a, const KiwiServer& b) {
        if ((a.distanceKm < 0) != (b.distanceKm < 0)) return b.distanceKm < 0;
        return a.distanceKm < b.distanceKm;
    });
}

}  // namespace dect2
