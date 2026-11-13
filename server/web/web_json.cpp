// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "web_json.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace onair_web {

namespace {
struct Reader {
    const std::string& s;
    size_t i = 0;
    int nodes = 0;
    std::string err;
    explicit Reader(const std::string& t) : s(t) {}
    bool fail(const char* e) { if (err.empty()) err = e; return false; }
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++; }
    bool lit(const char* w) {
        size_t k = 0;
        while (w[k]) { if (i + k >= s.size() || s[i + k] != w[k]) return false; k++; }
        i += k;
        return true;
    }
    static void putUtf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& v) {
        if (i + 4 > s.size()) return fail("short \\u escape");
        v = 0;
        for (int k = 0; k < 4; k++) {
            const char c = s[i + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        i += 4;
        return true;
    }
    // one UTF-8 sequence starting at s[i] (a byte >= 0x80): copied when well formed (no overlong forms, no surrogates, <= U+10FFFF)
    bool utf8(std::string& o) {
        const unsigned char c = (unsigned char)s[i];
        int len; unsigned cp, min;
        if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; min = 0x80; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; min = 0x800; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; min = 0x10000; }
        else return fail("invalid UTF-8");
        if (i + (size_t)len > s.size()) return fail("invalid UTF-8");
        for (int k = 1; k < len; k++) {
            const unsigned char d = (unsigned char)s[i + (size_t)k];
            if ((d & 0xC0) != 0x80) return fail("invalid UTF-8");
            cp = (cp << 6) | (d & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return fail("invalid UTF-8");
        o.append(s, i, (size_t)len);
        i += (size_t)len;
        return true;
    }
    bool string(std::string& o) {
        i++;   // the opening quote
        while (true) {
            if (i >= s.size()) return fail("unterminated string");
            const unsigned char c = (unsigned char)s[i];
            if (c == '"') { i++; break; }
            if (c < 0x20) return fail("control character in a string");
            if (c == '\\') {
                if (++i >= s.size()) return fail("unterminated string");
                const char e = s[i++];
                switch (e) {
                case '"': o += '"'; break;
                case '\\': o += '\\'; break;
                case '/': o += '/'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xDC00 && cp <= 0xDFFF) return fail("unpaired surrogate");
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        unsigned lo;
                        if (i + 2 > s.size() || s[i] != '\\' || s[i + 1] != 'u') return fail("unpaired surrogate");
                        i += 2;
                        if (!hex4(lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return fail("unpaired surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    if (cp == 0) return fail("NUL in a string");
                    putUtf8(o, cp);
                    break;
                }
                default: return fail("bad escape");
                }
            } else if (c >= 0x80) {
                if (!utf8(o)) return false;
            } else { o += (char)c; i++; }
            if (o.size() > kJsonMaxString) return fail("string too long");
        }
        return true;
    }
    bool number(double& v) {
        const size_t st = i;
        if (i < s.size() && s[i] == '-') i++;
        if (i >= s.size()) return fail("bad number");
        if (s[i] == '0') i++;
        else if (s[i] >= '1' && s[i] <= '9') { while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++; }
        else return fail("bad number");
        if (i < s.size() && s[i] == '.') {
            i++;
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("bad number");
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            i++;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("bad number");
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        }
        if (i - st > 40) return fail("number too long");
        const std::string t = s.substr(st, i - st);
        char* end = nullptr;
        v = std::strtod(t.c_str(), &end);
        if (!end || *end || !std::isfinite(v)) return fail("number out of range");
        return true;
    }
    bool value(Json& out, int depth) {
        if (++nodes > kJsonMaxNodes) return fail("too many values");
        ws();
        if (i >= s.size()) return fail("unexpected end");
        const char c = s[i];
        if (c == '{' || c == '[') {
            if (depth >= kJsonMaxDepth) return fail("nested too deep");
            const bool obj = c == '{';
            out.kind = obj ? Json::Obj : Json::Arr;
            i++;
            ws();
            const char close = obj ? '}' : ']';
            if (i < s.size() && s[i] == close) { i++; return true; }
            while (true) {
                if (obj) {
                    ws();
                    if (i >= s.size() || s[i] != '"') return fail("expected a key");
                    std::string k;
                    if (!string(k)) return false;
                    ws();
                    if (i >= s.size() || s[i] != ':') return fail("expected ':'");
                    i++;
                    Json v;
                    if (!value(v, depth + 1)) return false;
                    if (out.obj.count(k)) return fail("repeated key");
                    out.obj.emplace(std::move(k), std::move(v));
                } else {
                    Json v;
                    if (!value(v, depth + 1)) return false;
                    out.arr.push_back(std::move(v));
                }
                ws();
                if (i >= s.size()) return fail("unexpected end");
                if (s[i] == ',') { i++; continue; }
                if (s[i] == close) { i++; return true; }
                return fail(obj ? "expected ',' or '}'" : "expected ',' or ']'");
            }
        }
        if (c == '"') { out.kind = Json::Str; return string(out.str); }
        if (c == 't') { if (!lit("true")) return fail("bad literal"); out.kind = Json::Bool; out.b = true; return true; }
        if (c == 'f') { if (!lit("false")) return fail("bad literal"); out.kind = Json::Bool; out.b = false; return true; }
        if (c == 'n') { if (!lit("null")) return fail("bad literal"); out.kind = Json::Null; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) { out.kind = Json::Num; return number(out.num); }
        return fail("unexpected character");
    }
};
} // namespace

bool jsonParse(const std::string& text, Json& out, std::string& err) {
    out = Json();
    if (text.size() > kJsonMaxInput) { err = "message too long"; return false; }
    Reader r(text);
    Json v;
    if (!r.value(v, 0)) { err = r.err; return false; }
    r.ws();
    if (r.i != text.size()) { err = "data after the value"; return false; }
    out = std::move(v);
    return true;
}

std::string jsonEsc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20 || c == 0x7F) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

std::string jsonNumber(double v) {
    if (!std::isfinite(v)) return "null";
    char b[32];
    snprintf(b, sizeof b, "%.10g", v);
    return b;
}

} // namespace onair_web
