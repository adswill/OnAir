// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SAME / EAS message parsing and the tables: see same_proto.h.
#include "dect2/same/same_proto.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace dect2 {
namespace same {

namespace {
// the user's county table is state, so it stays here; the parsing is Rust (same_proto_rs.cpp)
struct Counties {
    std::mutex mu;
    std::map<int, std::string> names;   // state * 1000 + county
};
Counties& counties() { static Counties c; return c; }
} // namespace

// Parses county CSV text into the table's rows (same_proto_rs.cpp, over rust/onair_same)
std::map<int, std::string> parseCountyCsv(const std::string& text);

size_t loadCountyCsv(const std::string& text, std::string* err) {
    std::map<int, std::string> t = parseCountyCsv(text);
    if (t.empty()) { if (err) *err = "no county lines found (expected SSCCC,Name or SS,CCC,Name or the Census national_county.txt)"; return 0; }
    std::lock_guard<std::mutex> lk(counties().mu);
    counties().names = std::move(t);
    return counties().names.size();
}

size_t loadCountyFile(const std::string& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot open " + path; return 0; }
    std::ostringstream ss;
    ss << f.rdbuf();
    return loadCountyCsv(ss.str(), err);
}

size_t countyCount() { std::lock_guard<std::mutex> lk(counties().mu); return counties().names.size(); }
void clearCounties() { std::lock_guard<std::mutex> lk(counties().mu); counties().names.clear(); }
std::string countyNameOf(int state, int county) {
    std::lock_guard<std::mutex> lk(counties().mu);
    const auto it = counties().names.find(state * 1000 + county);
    return it == counties().names.end() ? std::string() : it->second;
}

} // namespace same
} // namespace dect2
