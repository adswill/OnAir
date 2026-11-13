// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The airband channel list as text for the settings file: see airband_store.h.
#include "dect2/airband/airband_store.h"
#include <cstdio>
#include <cstdlib>

namespace dect2 {
namespace {
bool plain(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_' || c == '.' || c == '/' || c >= 0x80; }
int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

std::string esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (plain(c)) o += (char)c;
        else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}
std::string unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) { o += (char)(hexv(s[i + 1]) * 16 + hexv(s[i + 2])); i += 2; }
        else o += s[i];
    }
    return o;
}
std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> v;
    size_t p = 0;
    while (p <= s.size()) {
        size_t e = s.find(sep, p);
        if (e == std::string::npos) e = s.size();
        v.push_back(s.substr(p, e - p));
        p = e + 1;
    }
    return v;
}
void add(std::vector<AirbandChannel>& v, const std::vector<std::string>& f, bool escaped) {
    if (f.size() < 5) return;
    AirbandChannel c;
    c.freqHz = atof(f[0].c_str()); c.is833 = f[1] == "1"; c.muted = f[2] == "1"; c.solo = f[3] == "1"; c.priority = f[4] == "1";
    if (f.size() > 5) c.label = escaped ? unesc(f[5]) : f[5];
    if (escaped && f.size() > 6 && !f[6].empty()) { char* e = nullptr; const double q = strtod(f[6].c_str(), &e); if (e && !*e && q >= -5 && q <= 45) c.squelchDb = (float)q; }
    if (airbandOnRaster(c.freqHz, c.is833)) v.push_back(c);
}
}   // namespace

std::string airbandEncode(const std::vector<AirbandChannel>& v) {
    std::string s = "v3:";
    for (const auto& c : v) {
        char b[96];
        snprintf(b, sizeof b, "%.1f,%d,%d,%d,%d,", c.freqHz, c.is833 ? 1 : 0, c.muted ? 1 : 0, c.solo ? 1 : 0, c.priority ? 1 : 0);
        s += b;
        s += esc(c.label);
        if (c.squelchDb > -50) { snprintf(b, sizeof b, ",%.1f", c.squelchDb); s += b; }
        s += ';';
    }
    return s;
}

std::vector<AirbandChannel> airbandDecode(const std::string& s) {
    std::vector<AirbandChannel> v;
    if (s.compare(0, 3, "v2:") == 0 || s.compare(0, 3, "v3:") == 0) {
        for (const auto& rec : split(s.substr(3), ';')) if (!rec.empty()) add(v, split(rec, ','), true);
        return v;
    }
    if (s.find('|') != std::string::npos) {   // macOS NSUserDefaults and old saves: fields split by '|', channels by ';'
        for (const auto& rec : split(s, ';')) add(v, split(rec, '|'), false);
        return v;
    }
    for (const auto& rec : split(s, '\x1e')) if (!rec.empty()) add(v, split(rec, '\x1f'), false);   // 0.2.5 and 0.2.6
    return v;
}
}   // namespace dect2
