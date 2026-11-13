// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Receiver strip helpers: label, UTF-8 safe ellipsis, chip width, Ctrl+digit, scroll-into-view.
#include "../../app/state/rxbar.h"
#include <cstdio>
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
int main() {
    check(rxbar::label(1, "ADS-B", 1090.0) == "1 \xC2\xB7 ADS-B \xC2\xB7 1090.000 MHz", "label");
    check(rxbar::ellipsize("abcdef", 6) == "abcdef", "fits");
    check(rxbar::ellipsize("abcdef", 4) == "abc\xE2\x80\xA6", "cut");
    check(rxbar::ellipsize("abcdef", 1) == "\xE2\x80\xA6", "one char");
    check(rxbar::ellipsize("abc", 0).empty(), "zero");
    check(rxbar::ellipsize("\xC3\xA4\xC3\xB6\xC3\xBC\xC3\x9F", 3) == "\xC3\xA4\xC3\xB6\xE2\x80\xA6", "multibyte not split");
    check(rxbar::chipWidth(10, 5, 40, 100) == 40 && rxbar::chipWidth(200, 5, 40, 100) == 100 && rxbar::chipWidth(50, 5, 40, 100) == 55, "chip width clamp");
    check(rxbar::indexForDigit(1, 3) == 0 && rxbar::indexForDigit(3, 3) == 2 && rxbar::indexForDigit(4, 3) == -1 && rxbar::indexForDigit(0, 3) == -1, "digit map");
    check(rxbar::scrollToShow(0, 0, 50, 100) == 0 && rxbar::scrollToShow(0, 80, 150, 100) == 50 && rxbar::scrollToShow(100, 20, 60, 100) == 20, "scroll");
    if (!fails) printf("ok\n");
    return fails;
}
