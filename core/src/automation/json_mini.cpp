// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/automation/json_mini.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dect2::automation::json {

namespace {
constexpr size_t kMaxText = 1u << 20;
constexpr int kMaxDepth = 32;

struct P {
    const std::string& t;
    size_t i = 0;
    std::string err;
    explicit P(const std::string& s) : t(s) {}
    bool fail(const char* m) {
        if (err.empty()) {
            int line = 1;
            for (size_t k = 0; k < i && k < t.size(); k++) if (t[k] == '\n') line++;
            err = "line " + std::to_string(line) + ": " + m;
        }
        return false;
    }
    void ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) i++; }
    static void utf8(std::string& o, uint32_t c) {
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
        else { o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 63)); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
    }
    bool hex4(uint32_t& v) {
        if (i + 4 > t.size()) return fail("bad \\u escape");
        v = 0;
        for (int k = 0; k < 4; k++) {
            const char c = t[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }
    bool str(std::string& o) {
        if (i >= t.size() || t[i] != '"') return fail("expected a string");
        i++;
        while (i < t.size()) {
            const char c = t[i++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return fail("control character in a string");
            if (c != '\\') { o += c; continue; }
            if (i >= t.size()) break;
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
                uint32_t v;
                if (!hex4(v)) return false;
                if (v >= 0xD800 && v < 0xDC00) {
                    uint32_t lo;
                    if (i + 1 < t.size() && t[i] == '\\' && t[i + 1] == 'u') { i += 2; if (!hex4(lo)) return false; if (lo >= 0xDC00 && lo < 0xE000) v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00); else v = 0xFFFD; }
                    else v = 0xFFFD;
                } else if (v >= 0xDC00 && v < 0xE000) v = 0xFFFD;
                utf8(o, v);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool value(Value& v, int depth) {
        if (depth > kMaxDepth) return fail("nested too deep");
        ws();
        if (i >= t.size()) return fail("unexpected end");
        const char c = t[i];
        if (c == '{') {
            i++; v = Value::object(); ws();
            if (i < t.size() && t[i] == '}') { i++; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (i >= t.size() || t[i] != ':') return fail("expected ':'");
                i++;
                Value x;
                if (!value(x, depth + 1)) return false;
                v.o[k] = std::move(x);
                ws();
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == '}') { i++; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            i++; v = Value::array(); ws();
            if (i < t.size() && t[i] == ']') { i++; return true; }
            for (;;) {
                Value x;
                if (!value(x, depth + 1)) return false;
                v.a.push_back(std::move(x));
                if (v.a.size() > 100000) return fail("array too long");
                ws();
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == ']') { i++; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v = Value::string({}); return str(v.s); }
        if (t.compare(i, 4, "true") == 0) { i += 4; v = Value::boolean(true); return true; }
        if (t.compare(i, 5, "false") == 0) { i += 5; v = Value::boolean(false); return true; }
        if (t.compare(i, 4, "null") == 0) { i += 4; v = Value(); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t j = i;
            if (t[j] == '-') j++;
            while (j < t.size() && ((t[j] >= '0' && t[j] <= '9') || t[j] == '.' || t[j] == 'e' || t[j] == 'E' || t[j] == '+' || t[j] == '-')) j++;
            const std::string num = t.substr(i, j - i);
            char* end = nullptr;
            const double d = std::strtod(num.c_str(), &end);
            if (!end || *end || !std::isfinite(d)) return fail("bad number");
            i = j; v = Value::number(d);
            return true;
        }
        return fail("unexpected character");
    }
};

void dumpTo(const Value& v, bool pretty, int ind, std::string& o) {
    auto nl = [&](int n) { if (pretty) { o += '\n'; o.append((size_t)n * 2, ' '); } };
    switch (v.type) {
    case Value::Null: o += "null"; break;
    case Value::Bool: o += v.b ? "true" : "false"; break;
    case Value::Num: {
        char b[40];
        if (std::fabs(v.n) < 1e15 && v.n == std::floor(v.n)) snprintf(b, sizeof b, "%.0f", v.n);
        else snprintf(b, sizeof b, "%.12g", v.n);
        o += b;
        break;
    }
    case Value::Str: o += '"'; o += escape(v.s); o += '"'; break;
    case Value::Arr:
        if (v.a.empty()) { o += "[]"; break; }
        o += '[';
        for (size_t k = 0; k < v.a.size(); k++) { if (k) o += ','; nl(ind + 1); dumpTo(v.a[k], pretty, ind + 1, o); }
        nl(ind); o += ']';
        break;
    case Value::Obj: {
        if (v.o.empty()) { o += "{}"; break; }
        o += '{';
        bool first = true;
        for (const auto& kv : v.o) {
            if (!first) o += ',';
            first = false;
            nl(ind + 1);
            o += '"'; o += escape(kv.first); o += pretty ? "\": " : "\":";
            dumpTo(kv.second, pretty, ind + 1, o);
        }
        nl(ind); o += '}';
        break;
    }
    }
}
}  // namespace

bool parse(const std::string& text, Value& out, std::string& err) {
    if (text.size() > kMaxText) { err = "file too large"; return false; }
    P p(text);
    Value v;
    if (!p.value(v, 0)) { err = p.err; return false; }
    p.ws();
    if (p.i != text.size()) { p.fail("extra text after the value"); err = p.err; return false; }
    out = std::move(v);
    return true;
}

std::string dump(const Value& v, bool pretty) {
    std::string o;
    dumpTo(v, pretty, 0, o);
    return o;
}

std::string escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c >= 0x80) {   // valid UTF-8 passes, anything else becomes U+FFFD (a webhook must get valid JSON)
            const int n = c >= 0xF0 && c < 0xF5 ? 3 : c >= 0xE0 && c < 0xF0 ? 2 : c >= 0xC2 && c < 0xE0 ? 1 : -1;
            bool ok = n > 0;
            for (int k = 1; ok && k <= n; k++) ok = i + (size_t)k < s.size() && ((unsigned char)s[i + (size_t)k] & 0xC0) == 0x80;
            if (ok) { o.append(s, i, (size_t)n + 1); i += (size_t)n; }
            else o += "\xEF\xBF\xBD";
            continue;
        }
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        case '\b': o += "\\b"; break;
        case '\f': o += "\\f"; break;
        default:
            if (c < 0x20 || c == 0x7F) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += (char)c;
        }
    }
    return o;
}

}  // namespace dect2::automation::json
