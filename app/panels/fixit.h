// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "How to fix": one table that maps a failure (a text the app shows, found by a pattern) to a plain explanation, numbered steps for this
// computer's OS and buttons for the actions that help (open a page, copy a command, open the codecs folder, copy the diagnostics).
// Pure data and lookup (no ImGui): the popup that shows it is in diag_ui.cpp. All texts go through tr() when shown.
#pragma once
#include <string>
#include <vector>

namespace fixit {

enum Os { OsMac = 1, OsWin = 2, OsLinux = 4, OsAll = 7 };
enum class Act { OpenUrl, CopyText, OpenCodecs, OpenDiagnostics };

struct Button {
    Act act;
    const char* label;   // N_(); nullptr ends the list
    const char* arg;     // the URL, or the text to copy
    int os;              // the Os bits it is offered on
};

struct Entry {
    const char* id;        // stable name
    const char* pattern;   // matched case-insensitively anywhere in the message
    const char* title;     // N_()
    const char* why;       // N_(): what it means, in plain words
    const char* any[6];    // N_() steps for every OS that has none of its own; nullptr ends the list
    const char* mac[6];
    const char* win[6];
    const char* lin[6];
    Button buttons[3];
};

const Entry* table(int& count);
const Entry* byId(const std::string& id);
const Entry* lookup(const std::string& message);   // the first entry whose pattern occurs in message, nullptr when none
int currentOs();                                   // OsMac / OsWin / OsLinux of this build
std::vector<const char*> stepsFor(const Entry& e, int os);   // the OS's own steps, else the common ones

}
