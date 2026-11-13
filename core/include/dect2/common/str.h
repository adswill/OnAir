// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Small ASCII string helpers shared by parsers and exporters: case folding, trimming, splitting,
// hex encoding and JSON string escaping. All are locale-independent in the "C" locale the programs run in
// (they call tolower/toupper/isspace on unsigned chars exactly as the per-file copies they replace did).
#pragma once
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {
namespace str {

// ASCII lower-case copy of s (tolower per unsigned char).
inline std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
// ASCII upper-case copy of s (toupper per unsigned char).
inline std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }

// Strip any of the characters in `chars` from both ends (all-blank input gives an empty string).
inline std::string trimChars(const std::string& s, const char* chars) {
    const size_t a = s.find_first_not_of(chars);
    if (a == std::string::npos) return std::string();
    return s.substr(a, s.find_last_not_of(chars) - a + 1);
}
// Strip leading and trailing whitespace as classified by isspace (space, \t \n \v \f \r).
inline std::string trimSpace(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// Split on a single delimiter character, keeping empty fields ("a,,b" -> a, "", b; "" -> one empty field).
inline std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) { if (c == sep) { v.push_back(cur); cur.clear(); } else cur += c; }
    v.push_back(cur);
    return v;
}

// Hex dump of n bytes: two digits per byte, upper- or lower-case, optionally separated by `sep` (0 = none), at most `maxBytes` bytes.
inline std::string toHex(const uint8_t* p, size_t n, bool upperCase = true, char sep = 0, size_t maxBytes = (size_t)-1) {
    static const char* du = "0123456789ABCDEF";
    static const char* dl = "0123456789abcdef";
    const char* d = upperCase ? du : dl;
    std::string s;
    for (size_t i = 0; i < n && i < maxBytes; i++) {
        if (sep && i) s += sep;
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

// JSON string content (RFC 8259 section 7): escapes " \ and control characters (\n \r \t short forms, others \u00xx); no surrounding quotes.
inline std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o.push_back((char)c);
        }
    }
    return o;
}
// Same as jsonEscape, but also escapes DEL as \u007f and replaces every byte >= 0x80 by '?' (7-bit decoder texts, always valid UTF-8).
inline std::string jsonEscape7bit(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20 || c == 0x7f) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else if (c >= 0x80) o += '?';
            else o += (char)c;
        }
    }
    return o;
}

} // namespace str
} // namespace dect2
