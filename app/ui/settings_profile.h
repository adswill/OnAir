// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Settings profiles (Settings > Profiles): a named set of every setting of the Settings window plus the keyboard and mouse bindings, kept as one
// text file in <data folder>/profiles/<name>.onairprofile, and exported or imported as the same file. No ImGui, no App: tests/app/test_settings.cpp.
//
//   # OnAir settings profile
//   [settings]
//   wfGamma=0.8
//   ...
//   [keys]
//   start = Space
//   ...
//
// A file without the [settings] / [keys] headers is read as settings only, so an older "Export settings" file loads as a profile too.
#pragma once
#include <cctype>
#include <string>
#include <vector>

namespace settings {

constexpr const char* kProfileExt = ".onairprofile";

inline std::string profileCompose(const std::string& settingsText, const std::string& keysText) {
    std::string o = "# OnAir settings profile\n[settings]\n";
    o += settingsText;
    if (!o.empty() && o.back() != '\n') o += '\n';
    o += "[keys]\n";
    o += keysText;
    if (!o.empty() && o.back() != '\n') o += '\n';
    return o;
}

// false when the text has neither section nor a key=value line
inline bool profileSplit(const std::string& text, std::string& settingsText, std::string& keysText) {
    settingsText.clear(); keysText.clear();
    int sect = 0;   // 0 before any header (settings), 1 settings, 2 keys, 3 an unknown section (skipped)
    bool any = false;
    size_t p = 0;
    while (p <= text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "[settings]") { sect = 1; any = true; continue; }
        if (line == "[keys]") { sect = 2; any = true; continue; }
        if (line.size() > 2 && line[0] == '[' && line.back() == ']') { sect = 3; continue; }
        if (line.empty() || line[0] == '#') continue;
        if (line.find('=') == std::string::npos) continue;
        any = true;
        if (sect == 2) keysText += line + "\n";
        else if (sect != 3) settingsText += line + "\n";
    }
    return any;
}

// A profile name the user typed -> a safe file name (letters, digits, space, - and _ kept; the rest becomes _); empty when nothing is left
inline std::string profileFileName(const std::string& name) {
    std::string o;
    for (unsigned char c : name) {
        if (std::isalnum(c) || c == ' ' || c == '-' || c == '_') o += (char)c;
        else if (c >= 0x80) o += (char)c;   // UTF-8 names stay readable
        else o += '_';
    }
    while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
    size_t b = 0;
    while (b < o.size() && o[b] == ' ') b++;
    o = o.substr(b);
    if (o.size() > 60) o.resize(60);
    bool real = false;
    for (char c : o) if (c != '_' && c != ' ') real = true;
    return real ? o + kProfileExt : std::string();
}
// the name of a profile from its file name ("Night.onairprofile" -> "Night"); empty when it is not a profile file
inline std::string profileNameOf(const std::string& file) {
    const std::string ext = kProfileExt;
    if (file.size() <= ext.size() || file.compare(file.size() - ext.size(), ext.size(), ext) != 0) return std::string();
    return file.substr(0, file.size() - ext.size());
}

}  // namespace settings
