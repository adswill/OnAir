// Winlink B2 message format, MID, date, secure login answer, Q word encoding. After the public B2F notes and wl2k-go's fbb package
// (Copyright 2015-2016 Martin Hebnes Pedersen LA5NTA, MIT licence; see THIRD_PARTY.md). New C++ code.
//
// Parts ported from wl2k-go (https://github.com/la5nta/wl2k-go), under this notice:
//
// The MIT License (MIT)
// Copyright (c) 2014-2017 Martin Hebnes Pedersen (LA5NTA)
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
// CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
// SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#include "dect2/winlink/b2msg.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace dect2 {
namespace winlink {

namespace {

bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

// textproto canonical form: first letter and each letter after '-' upper case, the rest lower case
std::string canonicalKey(const std::string& k) {
    std::string o = k;
    bool up = true;
    for (char& c : o) {
        if (up) c = (char)std::toupper((unsigned char)c); else c = (char)std::tolower((unsigned char)c);
        up = c == '-';
    }
    return o;
}

std::string latin1ToUtf8(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x80) o += (char)c;
        else { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
    }
    return o;
}

int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string b64dec(const std::string& s) {
    static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    uint32_t acc = 0; int bits = 0;
    for (char c : s) {
        const char* p = strchr(A, c);
        if (!p || c == 0) continue;
        acc = (acc << 6) | (uint32_t)(p - A);
        bits += 6;
        if (bits >= 8) { bits -= 8; o += (char)((acc >> bits) & 0xFF); }
    }
    return o;
}

} // namespace

// ---------------------------------------------------------------- MD5 (RFC 1321)

void md5(const uint8_t* msg, size_t len, uint8_t out[16]) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476;
    std::vector<uint8_t> m(msg, msg + len);
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    const uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) m.push_back((uint8_t)(bits >> (8 * i)));
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)m[off + 4 * i] | ((uint32_t)m[off + 4 * i + 1] << 8) | ((uint32_t)m[off + 4 * i + 2] << 16) | ((uint32_t)m[off + 4 * i + 3] << 24);
        uint32_t a = h0, b = h1, c = h2, d = h3;
        for (int i = 0; i < 64; i++) {
            uint32_t f; int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            const uint32_t t = d;
            d = c; c = b;
            const uint32_t x = a + f + K[i] + w[g];
            b = b + ((x << S[i]) | (x >> (32 - S[i])));
            a = t;
        }
        h0 += a; h1 += b; h2 += c; h3 += d;
    }
    const uint32_t h[4] = {h0, h1, h2, h3};
    for (int i = 0; i < 4; i++) for (int k = 0; k < 4; k++) out[4 * i + k] = (uint8_t)(h[i] >> (8 * k));
}

// ---------------------------------------------------------------- fields

std::string B2Message::get(const std::string& key) const {
    for (const auto& h : headers) if (ieq(h.first, key)) return h.second;
    return "";
}

std::vector<std::string> B2Message::getAll(const std::string& key) const {
    std::vector<std::string> v;
    for (const auto& h : headers) if (ieq(h.first, key)) v.push_back(h.second);
    return v;
}

void B2Message::set(const std::string& key, const std::string& value) {
    headers.erase(std::remove_if(headers.begin(), headers.end(), [&](const std::pair<std::string, std::string>& h) { return ieq(h.first, key); }), headers.end());
    headers.emplace_back(key, value);
}

void B2Message::add(const std::string& key, const std::string& value) { headers.emplace_back(key, value); }

std::string B2Message::subject() const { return qDecodeTitle(get("Subject")); }

std::string b2Date(std::time_t when) {
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &when);
#else
    gmtime_r(&when, &tmv);
#endif
    char b[32];
    snprintf(b, sizeof b, "%04d/%02d/%02d %02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    return b;
}

std::string generateMid(const std::string& callsign, std::time_t when, uint32_t salt) {
    char b[96];
    snprintf(b, sizeof b, "%lld.%u-%s", (long long)when, salt, callsign.c_str());
    uint8_t d[16];
    md5((const uint8_t*)b, strlen(b), d);
    static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string o;
    uint32_t acc = 0; int bits = 0;
    for (int i = 0; i < 16 && o.size() < 12; i++) {
        acc = (acc << 8) | d[i];
        bits += 8;
        while (bits >= 5 && o.size() < 12) { bits -= 5; o += A[(acc >> bits) & 31]; }
    }
    return o;
}

std::string secureLoginResponse(const std::string& challenge, const std::string& password) {
    static const uint8_t kSalt[64] = {77, 197, 101, 206, 190, 249, 93, 200, 51, 243, 93, 237, 71, 94, 239, 138, 68, 108, 70, 185, 225, 137,
                                      217, 16, 51, 122, 193, 48, 194, 195, 198, 175, 172, 169, 70, 84, 61, 62, 104, 186, 114, 52, 61, 168,
                                      66, 129, 192, 208, 187, 249, 232, 193, 41, 113, 41, 45, 240, 16, 29, 228, 208, 228, 61, 20};
    std::vector<uint8_t> p(challenge.begin(), challenge.end());
    p.insert(p.end(), password.begin(), password.end());
    p.insert(p.end(), kSalt, kSalt + 64);
    uint8_t d[16];
    md5(p.data(), p.size(), d);
    int32_t pr = (int32_t)(d[3] & 0x3F);
    for (int i = 2; i >= 0; i--) pr = (pr << 8) | d[i];
    char b[24];
    snprintf(b, sizeof b, "%08d", pr);
    const std::string s = b;
    return s.substr(s.size() - 8);
}

std::string qEncodeTitle(const std::string& t) {
    bool plain = true;
    for (unsigned char c : t) if (c < 0x20 || c > 0x7E) { plain = false; break; }
    if (plain) return t;
    std::string o = "=?utf-8?q?";
    static const char* H = "0123456789ABCDEF";
    for (unsigned char c : t) {
        if (c == ' ') o += '_';
        else if (c > 0x20 && c < 0x7F && c != '=' && c != '?' && c != '_') o += (char)c;
        else { o += '='; o += H[c >> 4]; o += H[c & 15]; }
    }
    return o + "?=";
}

std::string qDecodeTitle(const std::string& t) {
    std::string o;
    size_t i = 0;
    bool lastWord = false;
    while (i < t.size()) {
        if (t.compare(i, 2, "=?") == 0) {
            const size_t q1 = t.find('?', i + 2);
            const size_t q2 = q1 == std::string::npos ? q1 : t.find('?', q1 + 1);
            const size_t end = q2 == std::string::npos ? q2 : t.find("?=", q2 + 1);
            if (end != std::string::npos) {
                std::string cs = t.substr(i + 2, q1 - i - 2);
                const char enc = (char)std::tolower((unsigned char)t[q1 + 1]);
                const std::string txt = t.substr(q2 + 1, end - q2 - 1);
                std::string raw;
                if (enc == 'q') {
                    for (size_t k = 0; k < txt.size(); k++) {
                        if (txt[k] == '_') raw += ' ';
                        else if (txt[k] == '=' && k + 2 < txt.size() + 0 && hexv(txt[k + 1]) >= 0 && hexv(txt[k + 2]) >= 0) { raw += (char)(hexv(txt[k + 1]) * 16 + hexv(txt[k + 2])); k += 2; }
                        else raw += txt[k];
                    }
                } else raw = b64dec(txt);
                for (auto& c : cs) c = (char)std::tolower((unsigned char)c);
                if (cs == "iso-8859-1" || cs == "latin1" || cs == "windows-1252") raw = latin1ToUtf8(raw);
                // white space between two encoded words is dropped (RFC 2047)
                if (lastWord) while (!o.empty() && (o.back() == ' ' || o.back() == '\t')) o.pop_back();
                o += raw;
                i = end + 2;
                lastWord = true;
                continue;
            }
        }
        if (t[i] != ' ' && t[i] != '\t') lastWord = false;
        o += t[i++];
    }
    return o;
}

// ---------------------------------------------------------------- message

B2Message makeMessage(const std::string& from, const std::vector<std::string>& to, const std::vector<std::string>& cc, const std::string& subject,
                      const std::string& body, const std::vector<B2File>& files, std::time_t when) {
    static uint32_t counter = 0;
    B2Message m;
    m.set("Mid", generateMid(from, when, ++counter));
    m.set("Date", b2Date(when));
    m.set("Type", "Private");
    m.set("From", from);
    for (const auto& t : to) m.add("To", t);
    for (const auto& c : cc) m.add("Cc", c);
    m.set("Subject", qEncodeTitle(subject.empty() ? std::string("No subject") : subject));
    m.set("Mbo", from);
    m.set("Content-Type", "text/plain; charset=ISO-8859-1");
    m.set("Content-Transfer-Encoding", "8bit");
    std::string b;
    for (size_t i = 0; i < body.size(); i++) {
        if (body[i] == '\n' && (i == 0 || body[i - 1] != '\r')) b += '\r';
        b += body[i];
    }
    m.body.assign(b.begin(), b.end());
    m.files = files;
    return m;
}

std::vector<uint8_t> encodeMessage(const B2Message& in) {
    B2Message m = in;
    m.set("Body", std::to_string(m.body.size()));
    m.headers.erase(std::remove_if(m.headers.begin(), m.headers.end(), [](const std::pair<std::string, std::string>& h) { return ieq(h.first, "File"); }), m.headers.end());
    for (const auto& f : m.files) m.add("File", std::to_string(f.data.size()) + " " + qEncodeTitle(f.name));
    std::string mid = m.mid();
    std::vector<std::pair<std::string, std::string>> rest;
    for (const auto& h : m.headers) if (!ieq(h.first, "Mid")) rest.emplace_back(canonicalKey(h.first), trim(h.second));
    std::stable_sort(rest.begin(), rest.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string head = "Mid: " + mid + "\r\n";
    for (const auto& h : rest) head += h.first + ": " + h.second + "\r\n";
    head += "\r\n";
    std::vector<uint8_t> o(head.begin(), head.end());
    o.insert(o.end(), m.body.begin(), m.body.end());
    if (!m.files.empty()) { o.push_back('\r'); o.push_back('\n'); }
    for (const auto& f : m.files) {
        o.insert(o.end(), f.data.begin(), f.data.end());
        o.push_back('\r'); o.push_back('\n');
    }
    return o;
}

bool parseMessage(const uint8_t* d, size_t n, B2Message& m, std::string* err) {
    m = B2Message();
    auto fail = [&](const std::string& w) { if (err) *err = w; return false; };
    size_t p = 0;
    while (p < n && (d[p] == ' ' || d[p] == '\t' || d[p] == '\r' || d[p] == '\n')) p++;
    // header lines up to an empty line
    for (;;) {
        if (p >= n) return fail("header not ended");
        size_t e = p;
        while (e < n && d[e] != '\n') e++;
        if (e >= n) return fail("header not ended");
        std::string line((const char*)d + p, e - p);
        p = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        if ((line[0] == ' ' || line[0] == '\t') && !m.headers.empty()) { m.headers.back().second += " " + trim(line); continue; }
        const size_t c = line.find(':');
        if (c == std::string::npos || c == 0) return fail("bad header line: " + line);
        m.headers.emplace_back(canonicalKey(trim(line.substr(0, c))), trim(line.substr(c + 1)));
    }
    if (m.mid().empty()) return fail("no Mid");
    auto section = [&](size_t len, std::vector<uint8_t>& out) {
        if (p + len > n) return false;
        out.assign(d + p, d + p + len);
        p += len;
        if (p + 1 < n && d[p] == '\r' && d[p + 1] == '\n') p += 2;
        else if (p < n && d[p] == '\n') p += 1;
        return true;
    };
    const long bodyLen = atol(m.get("Body").c_str());
    if (bodyLen < 0 || !section((size_t)bodyLen, m.body)) return fail("body shorter than its Body field");
    for (const std::string& v : m.getAll("File")) {
        const size_t sp = v.find(' ');
        if (sp == std::string::npos) return fail("bad File field: " + v);
        B2File f;
        f.name = qDecodeTitle(v.substr(sp + 1));
        const long len = atol(v.substr(0, sp).c_str());
        if (len < 0 || !section((size_t)len, f.data)) return fail("file shorter than its File field: " + f.name);
        m.files.push_back(std::move(f));
    }
    return true;
}

} // namespace winlink
} // namespace dect2
