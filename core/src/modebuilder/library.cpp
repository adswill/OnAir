// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode Builder library: the example definitions (data/modes/*.json, compiled in by CMake as generated/modebuilder_examples.inc) and the
// user's saved definitions in a folder.
#include "dect2/modebuilder/modebuilder.h"
#include "dect2/common/u8file.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#if __has_include("modebuilder_examples.inc")
#include "modebuilder_examples.inc"   // kModeExamples[]: { file name, JSON text }
#define ONAIR_MB_HAVE_EXAMPLES 1
#endif

namespace dect2::modebuilder {
namespace fs = std::filesystem;

namespace {
std::string str8(const fs::path& p) { const auto u = p.u8string(); return std::string(u.begin(), u.end()); }
}   // namespace

const std::vector<Example>& examples() {
    static const std::vector<Example> v = [] {
        std::vector<Example> e;
#ifdef ONAIR_MB_HAVE_EXAMPLES
        for (const auto& x : kModeExamples) e.push_back({x.file, x.text});
#endif
        std::sort(e.begin(), e.end(), [](const Example& a, const Example& b) { return a.file < b.file; });
        return e;
    }();
    return v;
}

bool readText(const std::string& path, std::string& out, std::string& err) {
    std::ifstream f(dect2::u8file(path), std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    if (out.size() > (1u << 20)) { err = "larger than 1 MiB"; return false; }
    return true;
}

bool writeText(const std::string& path, const std::string& text, std::string& err) {
    std::error_code ec;
    const fs::path p = dect2::u8file(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { err = "cannot write " + path; return false; }
        f << text;
        if (!f) { err = "cannot write " + path; return false; }
    }
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(p, ec); fs::rename(tmp, p, ec); }
    if (ec) { err = "cannot write " + path + ": " + ec.message(); return false; }
    return true;
}

std::vector<Saved> listFolder(const std::string& dir) {
    std::vector<Saved> v;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dect2::u8file(dir), ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".json") continue;
        Saved s;
        s.path = str8(e.path());
        std::string text, err;
        if (!readText(s.path, text, err)) { s.error = err; v.push_back(s); continue; }
        const ParseResult r = parseDefinition(text);
        s.id = r.def.id;
        s.name = r.def.name.empty() ? str8(e.path().stem()) : r.def.name;
        if (!r.ok) for (const Issue& i : r.issues) if (i.error) { s.error = i.text(); break; }
        v.push_back(s);
    }
    std::sort(v.begin(), v.end(), [](const Saved& a, const Saved& b) { return a.name < b.name; });
    return v;
}

bool saveDefinition(const std::string& dir, const Definition& d, const std::string& text, std::string& pathOut, std::string& err) {
    const std::string id = d.id.empty() ? makeId(d.name) : d.id;
    pathOut = str8(dect2::u8file(dir) / dect2::u8file(id + ".json"));
    return writeText(pathOut, text, err);
}

}   // namespace dect2::modebuilder
