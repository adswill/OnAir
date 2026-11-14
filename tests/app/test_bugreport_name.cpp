// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors
// The crash report's name in the bug report zip (app/panels/bugreport_logic.h).
#include "../../app/panels/bugreport_logic.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    const std::string cur = "OnAir 4.0.0 crashed: exception 0xc0000005, address OnAir.exe+0x10\nstack:\n";
    const std::string old = "OnAir 0.2.6-beta.587 crashed: exception 0xc0000005, address OnAir.exe+0x27f4b30\n";
    CHECK(bugreport::crashVersion(cur) == "4.0.0", "version of a current report");
    CHECK(bugreport::crashFileName(cur, "4.0.0") == "crash.txt", "this version's crash keeps its name");
    CHECK(bugreport::crashFileName(old, "4.0.0") == "crash-from-0.2.6-beta.587.txt", "an old crash is named by its version: %s",
          bugreport::crashFileName(old, "4.0.0").c_str());
    CHECK(bugreport::crashFileName("garbage", "4.0.0") == "crash-from-unknown_version.txt", "not a crash report");
    CHECK(bugreport::crashFileName("OnAir 4.0.0\ncrashed", "4.0.0") == "crash-from-unknown_version.txt", "crashed must be on the first line");
    CHECK(bugreport::crashFileName("OnAir 1/2\\x crashed: y", "4.0.0") == "crash-from-1_2_x.txt", "no path characters in the name");
    std::printf("bugreport_name: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
