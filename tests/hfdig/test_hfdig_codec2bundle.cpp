// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The codec2 library that ships with OnAir (third_party/codec2, built as libcodec2 next to the programs) is found with nothing installed and no
// environment variable: a copy of this program in an empty folder, with only the library copied next to it, must load it from there (and
// not from Homebrew or the system). Skipped when the build has no bundled codec2 (-DDECT2_BUNDLE_CODEC2=OFF) or on Windows.
// "test_hfdig_codec2bundle --probe" is the copy's mode: it prints the library it loaded.
#include "dect2/hfdig/hfdig_freedv.h"
#include "dect2/m17/m17_codec2.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#endif
namespace fs = std::filesystem;
using namespace dect2;
#include "../common/test_util.h"

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--probe")) {
        std::string p, v;
        if (!m17Codec2Available(&p)) { printf("none\n"); return 2; }
        M17Codec2 c(3200);
        if (!c.ok() || c.samplesPerFrame() != 160) { printf("unusable %s\n", p.c_str()); return 3; }
        const bool fv = hfdigFreedvLibrary(&v, &v);
        printf("%s\n", p.c_str());
        return fv ? 0 : 4;
    }
#ifdef _WIN32
    printf("skipped: Windows\n");
    return 0;
#else
    std::error_code ec;
    const fs::path exe = fs::canonical(argv[0], ec);
    if (ec) { printf("FAIL: cannot find the program itself\n"); return 1; }
    const fs::path dir = exe.parent_path();
    fs::path lib;
    for (const auto& n : codecFileNameList(true)) if (fs::exists(dir / n, ec)) { lib = dir / n; break; }
    if (lib.empty()) { printf("skipped: no codec2 library next to the test (bundled codec2 is off)\n"); return 0; }

    const fs::path tmp = fs::temp_directory_path() / ("onair_c2bundle_" + std::to_string((long)getpid()));
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "home", ec);
    fs::copy_file(exe, tmp / "probe", ec);
    CHECK(!ec, "copy the program: %s", ec.message().c_str());
    fs::permissions(tmp / "probe", fs::perms::owner_all, fs::perm_options::add, ec);
    // the library under the file name it is loaded by (a symlink is followed: the copy is the real file)
    const std::string libName = lib.filename().string();
    fs::copy_file(lib, tmp / libName, ec);
    CHECK(!ec, "copy the library: %s", ec.message().c_str());

    const std::string cmd = "env -u DECT2_CODEC2_PATH HOME='" + (tmp / "home").string() + "' XDG_DATA_HOME='" + (tmp / "home").string() + "' '" +
                            (tmp / "probe").string() + "' --probe 2>&1";
    FILE* f = popen(cmd.c_str(), "r");
    std::string out;
    if (f) { char b[1024]; while (fgets(b, sizeof b, f)) out += b; }
    const int rc = f ? pclose(f) : -1;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    printf("copy loaded: %s (exit %d)\n", out.c_str(), rc);
    CHECK(rc == 0, "the copy did not load codec2 from its own folder");
    const std::string want = fs::canonical(tmp, ec).string();
    CHECK(out.find(want) != std::string::npos || out.find(tmp.string()) != std::string::npos, "loaded from elsewhere than next to the program: %s", out.c_str());
    fs::remove_all(tmp, ec);
    printf(fails ? "FAILED\n" : "ok\n");
    return fails ? 1 : 0;
#endif
}
