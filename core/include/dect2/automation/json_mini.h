// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A small JSON reader and writer for the automation rules and recipes: bounded depth and size, no exceptions, UTF-8 passed through.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2::automation::json {

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
    static Value string(std::string v) { Value x; x.type = Str; x.s = std::move(v); return x; }
    static Value array() { Value x; x.type = Arr; return x; }
    static Value object() { Value x; x.type = Obj; return x; }

    const Value* get(const std::string& k) const { auto it = o.find(k); return type == Obj && it != o.end() ? &it->second : nullptr; }
    std::string str(const std::string& k, const std::string& def = {}) const { const Value* v = get(k); return v && v->type == Str ? v->s : def; }
    double num(const std::string& k, double def = 0) const { const Value* v = get(k); return v && v->type == Num ? v->n : def; }
    bool flag(const std::string& k, bool def = false) const { const Value* v = get(k); return v && v->type == Bool ? v->b : def; }
    Value& set(const std::string& k, Value v) { type = Obj; return o[k] = std::move(v); }
    void push(Value v) { type = Arr; a.push_back(std::move(v)); }
};

// Parses text (at most 1 MiB, nesting at most 32). False with err ("line 3: expected ','") on any fault.
bool parse(const std::string& text, Value& out, std::string& err);
// pretty: two-space indentation, keys in order
std::string dump(const Value& v, bool pretty = false);
// the text of s as a JSON string literal without the quotes
std::string escape(const std::string& s);

}  // namespace dect2::automation::json
