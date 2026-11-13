// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OnAir Web: a small strict JSON reader for the messages browsers send, and the escaping the server's replies need. Self-contained (no other
// OnAir code) so the parser can be fuzzed on its own (tests/server/test_web_proto.cpp).
//
// Limits (anything over them is a parse error, never a partial result): input <= kJsonMaxInput bytes, nesting depth <= kJsonMaxDepth, at most
// kJsonMaxNodes values in total, strings <= kJsonMaxString bytes after unescaping, numbers finite and in the plain JSON grammar, valid UTF-8,
// no control characters in strings, no unpaired surrogates, nothing after the top value but white space.
#pragma once
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace onair_web {

constexpr size_t kJsonMaxInput = 4096;
constexpr int kJsonMaxDepth = 4;
constexpr int kJsonMaxNodes = 96;
constexpr size_t kJsonMaxString = 1024;

struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;   // a repeated key is an error

    const Json* get(const std::string& k) const {
        if (kind != Obj) return nullptr;
        auto it = obj.find(k);
        return it == obj.end() ? nullptr : &it->second;
    }
};

// false and err set (a short English reason) when the text is not one valid JSON value within the limits
bool jsonParse(const std::string& text, Json& out, std::string& err);

// the content of a JSON string (no quotes): ", \ and control characters escaped; bytes >= 0x80 copied (the server only writes valid UTF-8)
std::string jsonEsc(const std::string& s);
// a number as JSON (null for NaN / infinity), at most 10 significant digits
std::string jsonNumber(double v);

} // namespace onair_web
