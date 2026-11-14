// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The SITOR text history (sitor/sitor_history.h): a growing and a sliding receiver text append only what is new, a retune keeps the old text
// and adds a marker, a restarted receiver text is appended whole, and clear empties it.
#include "dect2/sitor/sitor_history.h"
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    SitorHistory h;
    h.update("HELLO");
    h.update("HELLO WORLD");
    CHECK(h.text == "HELLO WORLD", "grow: %s", h.text.c_str());
    h.update("HELLO WORLD");
    CHECK(h.text == "HELLO WORLD", "same: %s", h.text.c_str());
    h.update("LLO WORLD AGAIN");   // the receiver dropped the head of its text
    CHECK(h.text == "HELLO WORLD AGAIN", "slide: %s", h.text.c_str());
    h.retune("--- 4210.0 kHz 12:00:00 UTC ---");
    h.update("");
    h.update("NEW TEXT");
    CHECK(h.text == "HELLO WORLD AGAIN\n--- 4210.0 kHz 12:00:00 UTC ---\nNEW TEXT", "retune: %s", h.text.c_str());
    h.update("XYZ");   // no overlap: a restart, appended whole
    CHECK(h.text.size() > 8 && h.text.substr(h.text.size() - 3) == "XYZ", "restart");
    h.clear();
    CHECK(h.text.empty() && h.last.empty(), "clear");
    for (int i = 0; i < 3000; i++) h.update(std::string(100, 'A' + i % 26) + std::to_string(i));
    CHECK(h.text.size() <= SitorHistory::kMax, "cap %zu", h.text.size());
    printf("sitor history ok\n");
    return 0;
}
