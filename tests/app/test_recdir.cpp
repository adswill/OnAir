// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Recordings folder resolution (app/state/recdir_core.cpp): default, custom, unwritable fallback, trailing slash, spaces, non-ASCII.
#include "../../app/state/recdir.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static fs::path P(const std::string& u) { return fs::path(reinterpret_cast<const char8_t*>(u.c_str())); }
int main() {
    const fs::path root = fs::temp_directory_path() / "onair_recdir_test";
    std::error_code ec; fs::remove_all(root, ec);
    const std::string base = root.string();
    bool fb = true;
    // default, created on demand
    CHECK(recdir::resolve("", base + "/def", &fb) == base + "/def"); CHECK(!fb); CHECK(fs::is_directory(root / "def"));
    // custom
    CHECK(recdir::resolve(base + "/mine", base + "/def", &fb) == base + "/mine"); CHECK(!fb); CHECK(fs::is_directory(root / "mine"));
    // trailing slashes
    CHECK(recdir::resolve(base + "/slash//", base + "/def", &fb) == base + "/slash"); CHECK(!fb);
    CHECK(recdir::normalise("/") == "/"); CHECK(recdir::normalise("C:\\") == "C:\\"); CHECK(recdir::normalise("a\\b\\") == "a\\b");
    // spaces
    CHECK(recdir::resolve(base + "/my rec dir/", base + "/def", &fb) == base + "/my rec dir"); CHECK(fs::is_directory(root / "my rec dir"));
    // non-ASCII (UTF-8)
    const std::string uni = base + "/\xC4\x8D\xC4\x97\xC5\xA1 \xE5\xBD\x95\xE9\x9F\xB3";
    CHECK(recdir::resolve(uni, base + "/def", &fb) == uni); CHECK(!fb); CHECK(fs::is_directory(P(uni)));
    // unwritable: a path below a regular file cannot be created
    { std::ofstream f(root / "afile"); f << "x"; }
    CHECK(recdir::resolve(base + "/afile/sub", base + "/def", &fb) == base + "/def"); CHECK(fb);
    CHECK(recdir::freeBytes(base) > 0);
    fs::remove_all(root, ec);
    printf(fails ? "recdir: %d FAILED\n" : "recdir: ok\n", fails);
    return fails ? 1 : 0;
}
