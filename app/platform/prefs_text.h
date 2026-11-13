// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// One value of settings.conf (FilePrefs in platform_posix.cpp, the Linux and Windows settings file). The file holds one key=value
// a line, so a value with line breaks (the mesh channel keys, the FM scan list, ...) is written with \n, \r and \\ escapes and read
// back whole (#360: only the first MeshCore key came back after a restart). A file from before the escapes has no settingsFormat
// line and is read as it is, so its Windows paths keep their single backslashes.
#pragma once
#include <string>

namespace prefstext {

constexpr const char* kFormatKey = "settingsFormat";   // the first line of the file: settingsFormat=2
constexpr int kFormat = 2;                             // 2: values escaped; none (older files): values as they are

inline std::string escape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else o += c;
    }
    return o;
}

inline std::string unescape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 == s.size()) { o += s[i]; continue; }
        const char n = s[++i];
        if (n == 'n') o += '\n';
        else if (n == 'r') o += '\r';
        else if (n == '\\') o += '\\';
        else { o += '\\'; o += n; }   // not one of ours (a hand edit): kept as it is
    }
    return o;
}

// a value as read from the file, given the file's settingsFormat (0 when it has none)
inline std::string fromFile(const std::string& raw, int format) { return format >= kFormat ? unescape(raw) : raw; }

} // namespace prefstext
