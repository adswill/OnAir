// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Recording tags and notes (app/state/rec_tags.cpp): tag parsing, save/load round trip with tabs and line breaks in notes, search and filter.
#include "../../app/state/rec_tags.h"
#include <cstdio>
#include <filesystem>
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
    using namespace rectags;
    auto t = parseTags(" Airband, night;#ISS ,, airband\n");
    CHECK(t.size() == 3 && t[0] == "airband" && t[1] == "night" && t[2] == "iss");
    CHECK(joinTags(t) == "airband, night, iss");
    CHECK(parseTags("").empty() && parseTags(" , ; ").empty());

    const fs::path dir = fs::temp_directory_path() / "onair_rectags_test";
    std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir);
    Store s;
    CHECK(!load(dir.string(), s) && s.empty());
    set(s, "a.sigmf-meta", "Airband, night", "tower\tfreq\nsecond line \\ end");
    set(s, "b raw.cs8", "iss", "");
    set(s, "c.wav", "", "  ");           // nothing to keep
    CHECK(s.size() == 2);
    CHECK(save(dir.string(), s));
    Store r; CHECK(load(dir.string(), r));
    CHECK(r.size() == 2 && r["a.sigmf-meta"].note == "tower\tfreq\nsecond line \\ end");
    CHECK(r["a.sigmf-meta"].tags.size() == 2 && r["b raw.cs8"].tags[0] == "iss");
    // clearing an entry removes it
    set(r, "b raw.cs8", "", ""); CHECK(r.size() == 1);

    const Entry* a = &s["a.sigmf-meta"];
    CHECK(matches("a.sigmf-meta", a, ""));
    CHECK(matches("a.sigmf-meta", a, "AIRBAND"));       // tag, case-insensitive
    CHECK(matches("a.sigmf-meta", a, "tower night"));   // note + tag, every word
    CHECK(matches("a.sigmf-meta", a, "sigmf"));         // file name
    CHECK(!matches("a.sigmf-meta", a, "tower iss"));
    CHECK(matches("a.sigmf-meta", a, "#night") && !matches("a.sigmf-meta", a, "#nigh"));   // # = exact tag
    CHECK(!matches("x.wav", nullptr, "#night") && matches("x.wav", nullptr, "x.wav") && !matches("x.wav", nullptr, "tower"));
    auto c = tagCounts(s);
    CHECK(c.size() == 3);
    fs::remove_all(dir, ec);
    // unwritable folder
    CHECK(!save((dir / "missing").string(), s));
    printf(fails ? "rec_tags: %d FAILED\n" : "rec_tags: ok\n", fails);
    return fails ? 1 : 0;
}
