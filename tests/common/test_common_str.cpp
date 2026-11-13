// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Compares dect2/common/str.h against the ORIGINAL per-file helpers (embedded here as references) on random strings:
// lower/upper, the trim variants, split, toHex variants and the two JSON escapers.
#include "dect2/common/str.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <sstream>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// ---- originals ----
static std::string o_lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
static std::string o_lowerRig(const std::string& s) { std::string r = s; for (auto& c : r) c = (char)tolower((unsigned char)c); return r; }
static std::string o_upper(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }
static std::string o_trimWs4(const std::string& s) { size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n"); return a == std::string::npos ? "" : s.substr(a, b - a + 1); }
static std::string o_trimPasses(const std::string& s) { size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n"); return a == std::string::npos ? std::string() : s.substr(a, b - a + 1); }
static std::string o_trimIsspace(const std::string& s) { size_t a = 0, b = s.size(); while (a < b && std::isspace((unsigned char)s[a])) a++; while (b > a && std::isspace((unsigned char)s[b - 1])) b--; return s.substr(a, b - a); }
static std::string o_trimAdsb(std::string s) { while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back(); size_t a = 0; while (a < s.size() && isspace((unsigned char)s[a])) a++; return s.substr(a); }
static std::string o_trimChannelDb(const std::string& s) { size_t a = 0, b = s.size(); while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++; while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--; return s.substr(a, b - a); }
static std::string o_trimSpaceOnly(const std::string& s) { size_t a = 0, e = s.size(); while (a < e && s[a] == ' ') a++; while (e > a && s[e - 1] == ' ') e--; return s.substr(a, e - a); }
static std::string o_trimNetradio(const std::string& v) { size_t a = 0, b = v.size(); while (a < b && (v[a] == ' ' || v[a] == '\t')) a++; while (b > a && (v[b - 1] == ' ' || v[b - 1] == '\t')) b--; return v.substr(a, b - a); }
static std::vector<std::string> o_split(const std::string& s, char sep) { std::vector<std::string> v; std::string cur; for (char c : s) { if (c == sep) { v.push_back(cur); cur.clear(); } else cur += c; } v.push_back(cur); return v; }
static std::vector<std::string> o_splitTest(const std::string& s, char c) { std::vector<std::string> o; size_t p = 0; for (;;) { size_t e = s.find(c, p); o.push_back(s.substr(p, e == std::string::npos ? std::string::npos : e - p)); if (e == std::string::npos) break; p = e + 1; } return o; }
static std::string o_hexAero(const std::vector<uint8_t>& v) { static const char* d = "0123456789ABCDEF"; std::string s; for (uint8_t c : v) { s += d[c >> 4]; s += d[c & 15]; } return s; }
static std::string o_hexAdsb(const uint8_t* b, int nbits) { static const char* d = "0123456789ABCDEF"; std::string s; for (int i = 0; i < nbits / 8; i++) { s += d[b[i] >> 4]; s += d[b[i] & 15]; } return s; }
static std::string o_hexAnt(const uint8_t* p, size_t n) { std::string s; char b[4]; for (size_t i = 0; i < n; i++) { snprintf(b, sizeof b, "%02X", p[i]); s += b; if (i + 1 < n) s += ' '; } return s; }
static std::string o_hexUat(const uint8_t* d, int n) { std::string h; h.reserve((size_t)n * 2); char b[4]; for (int i = 0; i < n; i++) { snprintf(b, sizeof b, "%02X", d[i]); h += b; } return h; }
static std::string o_hexIridium(const std::vector<uint8_t>& v, size_t n) { std::string s; char t[4]; for (size_t i = 0; i < v.size() && i < n; i++) { snprintf(t, sizeof t, "%02x", v[i]); s += t; } return s; }
static std::string o_hexLora(const std::vector<uint8_t>& p) { std::string s; char b[4]; for (size_t i = 0; i < p.size() && i < 255; i++) { snprintf(b, sizeof b, "%02X", p[i]); if (i) s += ' '; s += b; } return s; }
static std::string o_hexVdl2(const uint8_t* p, size_t n, size_t max) { std::string s; char b[4]; for (size_t i = 0; i < n && i < max; i++) { snprintf(b, sizeof b, "%02X", p[i]); s += b; } if (n > max) s += "..."; return s; }
static std::string o_hexMesh(const uint8_t* p, size_t n, bool upper) { const char* d = upper ? "0123456789ABCDEF" : "0123456789abcdef"; std::string s; for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; } return s; }
static std::string o_jsonRemote(const std::string& s) { std::string o; for (unsigned char c : s) { switch (c) { case '"': o += "\\\""; break; case '\\': o += "\\\\"; break; case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break; default: if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; } else o.push_back((char)c); } } return o; }
static std::string o_jsonFeeds(const std::string& s) { std::string o; o.reserve(s.size() + 8); for (unsigned char c : s) { switch (c) { case '"': o += "\\\""; break; case '\\': o += "\\\\"; break; case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break; default: if (c < 0x20 || c == 0x7f) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; } else if (c >= 0x80) o += '?'; else o += (char)c; } } return o; }

int main() {
    std::mt19937 r(4242);
    static const char alpha[] = " \t\r\n\v\f,;AbZz09_-\"\\\x01\x1f\x7f\x80\xff@";
    for (int it = 0; it < 20000; it++) {
        std::string s(r() % 24, ' ');
        for (auto& c : s) c = (r() % 4 == 0) ? (char)r() : alpha[r() % (sizeof alpha - 1)];
        CHECK(o_lower(s) == str::lower(s) && o_lowerRig(s) == str::lower(s), "lower");
        CHECK(o_upper(s) == str::upper(s), "upper");
        CHECK(o_trimWs4(s) == str::trimChars(s, " \t\r\n") && o_trimPasses(s) == str::trimChars(s, " \t\r\n"), "trim4");
        CHECK(o_trimIsspace(s) == str::trimSpace(s) && o_trimAdsb(s) == str::trimSpace(s), "trimSpace");
        CHECK(o_trimChannelDb(s) == str::trimChars(s, " \t\r"), "trim3");
        CHECK(o_trimSpaceOnly(s) == str::trimChars(s, " "), "trim1");
        CHECK(o_trimNetradio(s) == str::trimChars(s, " \t"), "trim2");
        const char sep = (it & 1) ? ',' : ' ';
        CHECK(o_split(s, sep) == str::split(s, sep) && o_splitTest(s, sep) == str::split(s, sep), "split");
        CHECK(o_jsonRemote(s) == str::jsonEscape(s), "json");
        CHECK(o_jsonFeeds(s) == str::jsonEscape7bit(s), "json7bit");
        std::vector<uint8_t> v(r() % 300); for (auto& x : v) x = (uint8_t)r();
        const uint8_t* p = v.data(); const size_t n = v.size();
        CHECK(o_hexAero(v) == str::toHex(p, n), "hexAero");
        CHECK(o_hexAdsb(p, (int)(n * 8 + r() % 8)) == str::toHex(p, ((n * 8 + 0) / 8)), "hexAdsb(nbits multiple)");
        CHECK(o_hexAnt(p, n) == str::toHex(p, n, true, ' '), "hexAnt");
        CHECK(o_hexUat(p, (int)n) == str::toHex(p, n), "hexUat");
        { const size_t k = r() % 400; CHECK(o_hexIridium(v, k) == str::toHex(p, n, false, 0, k), "hexIridium"); }
        CHECK(o_hexLora(v) == str::toHex(p, n, true, ' ', 255), "hexLora");
        { const size_t m = r() % 40; std::string q = str::toHex(p, n, true, 0, m); if (n > m) q += "..."; CHECK(o_hexVdl2(p, n, m) == q, "hexVdl2"); }
        { const bool up = r() & 1; CHECK(o_hexMesh(p, n, up) == str::toHex(p, n, up), "hexMesh"); }
    }
    printf(fails ? "test_common_str: %d FAILED\n" : "test_common_str: OK\n", fails);
    return fails ? 1 : 0;
}
