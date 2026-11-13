// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "memory_json.h"
#include "dect2/common/u8file.h"
#include <cstdio>
#include <cstdlib>

namespace dect2::mj {

namespace {
struct P {
    const std::string& s;
    size_t i = 0;
    std::string err;
    explicit P(const std::string& t) : s(t) {}
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++; }
    bool fail(const char* m) { if (err.empty()) { err = std::string(m) + " at byte " + std::to_string(i); } return false; }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | cp >> 6); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | cp >> 12); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | cp >> 18); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& v) {
        if (i + 4 > s.size()) return false;
        v = 0;
        for (int k = 0; k < 4; k++) {
            const char c = s[i + (size_t)k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        i += 4;
        return true;
    }
    bool str(std::string& o) {
        i++;   // the opening quote
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { o += c; continue; }
            if (i >= s.size()) break;
            const char e = s[i++];
            switch (e) {
            case '"': o += '"'; break; case '\\': o += '\\'; break; case '/': o += '/'; break;
            case 'b': o += '\b'; break; case 'f': o += '\f'; break; case 'n': o += '\n'; break; case 'r': o += '\r'; break; case 't': o += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!hex4(cp)) return fail("bad \\u escape");
                if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {   // a surrogate pair
                    const size_t keep = i;
                    i += 2;
                    unsigned lo;
                    if (hex4(lo) && lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); else { i = keep; cp = 0xFFFD; }
                } else if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
                utf8(o, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool val(Value& v, int depth) {
        if (depth > 24) return fail("too deep");
        ws();
        if (i >= s.size()) return fail("unexpected end");
        const char c = s[i];
        if (c == '{') {
            v.kind = Value::Obj; i++; ws();
            if (i < s.size() && s[i] == '}') { i++; return true; }
            while (true) {
                ws();
                if (i >= s.size() || s[i] != '"') return fail("expected a key");
                std::string k;
                if (!str(k)) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return fail("expected ':'");
                i++;
                v.obj.emplace_back(std::move(k), Value());
                if (!val(v.obj.back().second, depth + 1)) return false;
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.kind = Value::Arr; i++; ws();
            if (i < s.size() && s[i] == ']') { i++; return true; }
            while (true) {
                v.arr.emplace_back();
                if (!val(v.arr.back(), depth + 1)) return false;
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v.kind = Value::Str; return str(v.str); }
        if (s.compare(i, 4, "true") == 0) { v.kind = Value::Bool; v.b = true; i += 4; return true; }
        if (s.compare(i, 5, "false") == 0) { v.kind = Value::Bool; v.b = false; i += 5; return true; }
        if (s.compare(i, 4, "null") == 0) { v.kind = Value::Null; i += 4; return true; }
        char* end = nullptr;
        const double d = strtod(s.c_str() + i, &end);
        if (end == s.c_str() + i) return fail("unexpected character");
        v.kind = Value::Num; v.num = d; i = (size_t)(end - s.c_str());
        return true;
    }
};
} // namespace

bool parse(const std::string& text, Value& out, std::string& err) {
    if (text.size() > (16u << 20)) { err = "file too large"; return false; }
    P p(text);
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) p.i = 3;   // a byte order mark
    const bool ok = p.val(out, 0);
    err = p.err;
    return ok;
}

std::string quote(const std::string& in) {
    const std::string s = u8valid(in);
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

} // namespace dect2::mj
