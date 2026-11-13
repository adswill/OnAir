// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A small JSON value, writer and parser for the TV recorder files (EPG cache, schedule, recording sidecars). Not a general library: numbers are
// doubles (exact for integers up to 2^53, which holds unix seconds and event ids), strings are UTF-8, nesting is limited so a hostile file
// cannot exhaust the stack.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace dect2::tvjson {

struct Value {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> a;
    std::map<std::string, Value> o;

    Value() = default;
    static Value boolean(bool v) { Value x; x.type = Bool; x.b = v; return x; }
    static Value number(double v) { Value x; x.type = Num; x.n = v; return x; }
    static Value str(std::string v) { Value x; x.type = Str; x.s = std::move(v); return x; }
    static Value array() { Value x; x.type = Arr; return x; }
    static Value object() { Value x; x.type = Obj; return x; }

    Value& set(const std::string& k, Value v) { type = Obj; o[k] = std::move(v); return *this; }
    Value& set(const std::string& k, const std::string& v) { return set(k, str(v)); }
    Value& set(const std::string& k, const char* v) { return set(k, str(v)); }
    Value& set(const std::string& k, double v) { return set(k, number(v)); }
    Value& set(const std::string& k, int64_t v) { return set(k, number((double)v)); }
    Value& set(const std::string& k, int v) { return set(k, number(v)); }
    Value& set(const std::string& k, bool v) { return set(k, boolean(v)); }
    Value& push(Value v) { type = Arr; a.push_back(std::move(v)); return *this; }

    const Value* get(const std::string& k) const { if (type != Obj) return nullptr; auto it = o.find(k); return it == o.end() ? nullptr : &it->second; }
    std::string getStr(const std::string& k, const std::string& d = "") const { const Value* v = get(k); return v && v->type == Str ? v->s : d; }
    double getNum(const std::string& k, double d = 0) const { const Value* v = get(k); return v && v->type == Num ? v->n : d; }
    int64_t getInt(const std::string& k, int64_t d = 0) const { const Value* v = get(k); return v && v->type == Num ? (int64_t)v->n : d; }
    bool getBool(const std::string& k, bool d = false) const { const Value* v = get(k); return v && v->type == Bool ? v->b : d; }
};

inline void escapeInto(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
            else out += (char)c;
        }
    }
    out += '"';
}

inline void write(const Value& v, std::string& out) {
    switch (v.type) {
    case Value::Null: out += "null"; break;
    case Value::Bool: out += v.b ? "true" : "false"; break;
    case Value::Num: {
        char b[40];
        if (v.n == (double)(int64_t)v.n && v.n > -9e15 && v.n < 9e15) snprintf(b, sizeof b, "%lld", (long long)v.n);
        else snprintf(b, sizeof b, "%.9g", v.n);
        out += b;
        break;
    }
    case Value::Str: escapeInto(out, v.s); break;
    case Value::Arr:
        out += '[';
        for (size_t i = 0; i < v.a.size(); i++) { if (i) out += ','; write(v.a[i], out); }
        out += ']';
        break;
    case Value::Obj: {
        out += '{';
        bool first = true;
        for (const auto& kv : v.o) { if (!first) out += ','; first = false; escapeInto(out, kv.first); out += ':'; write(kv.second, out); }
        out += '}';
        break;
    }
    }
}
inline std::string dump(const Value& v) { std::string s; write(v, s); return s; }

class Parser {
public:
    Parser(const char* p, size_t n) : p_(p), e_(p + n) {}
    bool parse(Value& out) {
        ws();
        if (!value(out, 0)) return false;
        ws();
        return p_ == e_;
    }
private:
    const char* p_; const char* e_;
    void ws() { while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) p_++; }
    bool lit(const char* w) { const size_t n = strlen(w); if ((size_t)(e_ - p_) < n || memcmp(p_, w, n)) return false; p_ += n; return true; }
    static void utf8(std::string& o, uint32_t cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | cp >> 6); o += (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | cp >> 12); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
        else { o += (char)(0xF0 | cp >> 18); o += (char)(0x80 | ((cp >> 12) & 63)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
    }
    bool hex4(uint32_t& v) {
        if (e_ - p_ < 4) return false;
        v = 0;
        for (int i = 0; i < 4; i++) { const char c = *p_++; v <<= 4; if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10; else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return false; }
        return true;
    }
    bool string(std::string& s) {
        if (p_ >= e_ || *p_ != '"') return false;
        p_++;
        while (p_ < e_) {
            const char c = *p_++;
            if (c == '"') return true;
            if (c != '\\') { s += c; continue; }
            if (p_ >= e_) return false;
            const char x = *p_++;
            switch (x) {
            case '"': s += '"'; break; case '\\': s += '\\'; break; case '/': s += '/'; break;
            case 'n': s += '\n'; break; case 'r': s += '\r'; break; case 't': s += '\t'; break; case 'b': s += '\b'; break; case 'f': s += '\f'; break;
            case 'u': {
                uint32_t cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && e_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                    p_ += 2; uint32_t lo;
                    if (!hex4(lo)) return false;
                    if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); else cp = 0xFFFD;
                } else if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
                utf8(s, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool value(Value& v, int depth) {
        if (depth > 32 || p_ >= e_) return false;
        const char c = *p_;
        if (c == '{') {
            p_++; v = Value::object(); ws();
            if (p_ < e_ && *p_ == '}') { p_++; return true; }
            for (;;) {
                ws(); std::string k;
                if (!string(k)) return false;
                ws();
                if (p_ >= e_ || *p_++ != ':') return false;
                ws();
                Value x;
                if (!value(x, depth + 1)) return false;
                v.o[k] = std::move(x);
                ws();
                if (p_ >= e_) return false;
                if (*p_ == ',') { p_++; continue; }
                if (*p_ == '}') { p_++; return true; }
                return false;
            }
        }
        if (c == '[') {
            p_++; v = Value::array(); ws();
            if (p_ < e_ && *p_ == ']') { p_++; return true; }
            for (;;) {
                ws(); Value x;
                if (!value(x, depth + 1)) return false;
                v.a.push_back(std::move(x));
                ws();
                if (p_ >= e_) return false;
                if (*p_ == ',') { p_++; continue; }
                if (*p_ == ']') { p_++; return true; }
                return false;
            }
        }
        if (c == '"') { v = Value(); v.type = Value::Str; return string(v.s); }
        if (c == 't') { if (!lit("true")) return false; v = Value::boolean(true); return true; }
        if (c == 'f') { if (!lit("false")) return false; v = Value::boolean(false); return true; }
        if (c == 'n') { if (!lit("null")) return false; v = Value(); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char* s = p_;
            if (*p_ == '-') p_++;
            while (p_ < e_ && ((*p_ >= '0' && *p_ <= '9') || *p_ == '.' || *p_ == 'e' || *p_ == 'E' || *p_ == '+' || *p_ == '-')) p_++;
            const std::string t(s, p_);
            char* end = nullptr;
            const double d = strtod(t.c_str(), &end);
            if (end != t.c_str() + t.size()) return false;
            v = Value::number(d);
            return true;
        }
        return false;
    }
};

inline bool parse(const std::string& text, Value& out) { return Parser(text.data(), text.size()).parse(out); }

} // namespace dect2::tvjson
