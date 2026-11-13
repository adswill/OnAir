// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A small full-JSON value for the MCP server: parser (depth and size limited, UTF-8 and \u escapes) and compact writer.
#include "dect2/mcp/mcp.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2 {
namespace mcp {

const Json* Json::get(const std::string& key) const {
    if (kind != Obj) return nullptr;
    for (const auto& kv : obj) if (kv.first == key) return &kv.second;
    return nullptr;
}

Json& Json::set(const std::string& key, Json v) {
    if (kind != Obj) { kind = Obj; obj.clear(); }
    for (auto& kv : obj) if (kv.first == key) { kv.second = std::move(v); return *this; }
    obj.emplace_back(key, std::move(v));
    return *this;
}

double Json::numOr(const std::string& key, double d) const {
    const Json* j = get(key);
    if (!j) return d;
    if (j->kind == Num) return j->num;
    if (j->kind == Str && !j->str.empty()) {   // a model may send "15.2" for a number
        char* e = nullptr;
        const double v = strtod(j->str.c_str(), &e);
        if (e && *e == 0 && std::isfinite(v)) return v;
    }
    return d;
}

std::string Json::strOr(const std::string& key, const std::string& d) const {
    const Json* j = get(key);
    if (!j) return d;
    if (j->kind == Str) return j->str;
    if (j->kind == Num) { char b[64]; snprintf(b, sizeof b, "%.15g", j->num); return b; }
    return d;
}

bool Json::boolOr(const std::string& key, bool d) const {
    const Json* j = get(key);
    if (!j) return d;
    if (j->kind == Bool) return j->b;
    if (j->kind == Str) { if (j->str == "true") return true; if (j->str == "false") return false; }
    return d;
}

namespace {

void esc(std::string& o, const std::string& s) {
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += (char)c;
        }
    }
    o += '"';
}

void dumpTo(std::string& o, const Json& j) {
    switch (j.kind) {
    case Json::Null: o += "null"; break;
    case Json::Bool: o += j.b ? "true" : "false"; break;
    case Json::Num: {
        if (!std::isfinite(j.num)) { o += "null"; break; }
        char b[48];
        if (j.num == std::floor(j.num) && std::fabs(j.num) < 1e15) snprintf(b, sizeof b, "%.0f", j.num);
        else snprintf(b, sizeof b, "%.10g", j.num);
        o += b;
        break;
    }
    case Json::Str: esc(o, j.str); break;
    case Json::Arr:
        o += '[';
        for (size_t i = 0; i < j.arr.size(); i++) { if (i) o += ','; dumpTo(o, j.arr[i]); }
        o += ']';
        break;
    case Json::Obj:
        o += '{';
        for (size_t i = 0; i < j.obj.size(); i++) { if (i) o += ','; esc(o, j.obj[i].first); o += ':'; dumpTo(o, j.obj[i].second); }
        o += '}';
        break;
    }
}

struct P {
    const std::string& t;
    size_t i = 0;
    std::string err;
    explicit P(const std::string& s) : t(s) {}
    bool fail(const char* m) { if (err.empty()) err = std::string(m) + " at byte " + std::to_string(i); return false; }
    void ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) i++; }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& v) {
        if (i + 4 > t.size()) return fail("short \\u escape");
        v = 0;
        for (int k = 0; k < 4; k++) {
            const char c = t[i + (size_t)k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        i += 4;
        return true;
    }
    bool str(std::string& o) {
        if (i >= t.size() || t[i] != '"') return fail("string expected");
        i++;
        while (i < t.size()) {
            const unsigned char c = (unsigned char)t[i++];
            if (c == '"') return true;
            if (c < 0x20) return fail("control character in string");
            if (c != '\\') { o += (char)c; continue; }
            if (i >= t.size()) return fail("bad escape");
            const char e = t[i++];
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
                unsigned v;
                if (!hex4(v)) return false;
                if (v >= 0xD800 && v < 0xDC00) {
                    if (i + 1 < t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                        i += 2;
                        unsigned lo;
                        if (!hex4(lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return fail("bad surrogate pair");
                        v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                    } else return fail("lone surrogate");
                } else if (v >= 0xDC00 && v <= 0xDFFF) return fail("lone surrogate");
                utf8(o, v);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool val(Json& o, int depth) {
        if (depth > 32) return fail("nested too deep");
        ws();
        if (i >= t.size()) return fail("value expected");
        const char c = t[i];
        if (c == '{') {
            i++;
            o = Json::object();
            ws();
            if (i < t.size() && t[i] == '}') { i++; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (i >= t.size() || t[i] != ':') return fail("':' expected");
                i++;
                Json v;
                if (!val(v, depth + 1)) return false;
                o.set(k, std::move(v));
                ws();
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == '}') { i++; return true; }
                return fail("',' or '}' expected");
            }
        }
        if (c == '[') {
            i++;
            o = Json::array();
            ws();
            if (i < t.size() && t[i] == ']') { i++; return true; }
            for (;;) {
                Json v;
                if (!val(v, depth + 1)) return false;
                o.arr.push_back(std::move(v));
                ws();
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == ']') { i++; return true; }
                return fail("',' or ']' expected");
            }
        }
        if (c == '"') { o = Json::string(""); return str(o.str); }
        if (t.compare(i, 4, "true") == 0) { i += 4; o = Json::boolean(true); return true; }
        if (t.compare(i, 5, "false") == 0) { i += 5; o = Json::boolean(false); return true; }
        if (t.compare(i, 4, "null") == 0) { i += 4; o = Json(); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const size_t s0 = i;
            if (t[i] == '-') i++;
            while (i < t.size() && ((t[i] >= '0' && t[i] <= '9') || t[i] == '.' || t[i] == 'e' || t[i] == 'E' || t[i] == '+' || t[i] == '-')) i++;
            const std::string n = t.substr(s0, i - s0);
            char* e = nullptr;
            const double v = strtod(n.c_str(), &e);
            if (!e || *e || !std::isfinite(v)) { i = s0; return fail("bad number"); }
            o = Json::number(v);
            return true;
        }
        return fail("unexpected character");
    }
};

} // namespace

std::string Json::dump() const {
    std::string o;
    dumpTo(o, *this);
    return o;
}

bool parseJson(const std::string& text, Json& out, std::string& err) {
    if (text.size() > 4u * 1024 * 1024) { err = "message too large"; return false; }
    P p(text);
    Json v;
    if (!p.val(v, 0)) { err = p.err; return false; }
    p.ws();
    if (p.i != text.size()) { err = "trailing characters at byte " + std::to_string(p.i); return false; }
    out = std::move(v);
    return true;
}

} // namespace mcp
} // namespace dect2
