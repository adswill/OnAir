// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The order of the CJK fonts merged into the UI fonts (app/platform/cjk_order.h): the fonts of the interface language first, so a Japanese
// line is not drawn half in a Korean font; every other language keeps the order from before.
#include "../../app/platform/cjk_order.h"
#include <cstdio>

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { failures++; std::printf("FAIL: %s\n", msg); } } while (0)

int main() {
    using V = std::vector<std::string>;
    const V ja = {"ja1", "ja2"}, zh = {"zh1"}, ko = {"ko1"};
    CHECK(plat::cjkFontOrder("ja", ja, zh, ko) == (V{"ja1", "ja2", "zh1", "ko1"}), "Japanese: Japanese, Chinese, Korean");
    CHECK(plat::cjkFontOrder("ja-JP", ja, zh, ko) == (V{"ja1", "ja2", "zh1", "ko1"}), "macOS language code");
    CHECK(plat::cjkFontOrder("ja_JP.UTF-8", ja, zh, ko) == (V{"ja1", "ja2", "zh1", "ko1"}), "POSIX locale");
    CHECK(plat::cjkFontOrder("zh-Hans", ja, zh, ko) == (V{"zh1", "ja1", "ja2", "ko1"}), "Chinese: Chinese, Japanese, Korean");
    CHECK(plat::cjkFontOrder("ko", ja, zh, ko) == (V{"ko1", "zh1", "ja1", "ja2"}), "Korean: the order from before");
    CHECK(plat::cjkFontOrder("en", ja, zh, ko) == (V{"ko1", "zh1", "ja1", "ja2"}), "English: the order from before");
    CHECK(plat::cjkFontOrder("", ja, zh, ko) == (V{"ko1", "zh1", "ja1", "ja2"}), "no language: the order from before");
    CHECK(plat::cjkFontOrder("j", ja, zh, ko) == (V{"ko1", "zh1", "ja1", "ja2"}), "a code shorter than two letters");
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("cjk_order: all passed\n");
    return 0;
}
