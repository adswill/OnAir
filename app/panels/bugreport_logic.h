// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors
// The name a crash report gets inside the bug report zip. crash.txt stays on disk until the next crash, so a
// report from an older version can still be there: it goes in as crash-from-<version>.txt so that nobody takes
// it for a crash of the version that saved the bug report.
#pragma once
#include <cctype>
#include <string>

namespace bugreport {

// the version in the first line of a crash report ("OnAir <version> crashed: ..."), or "" when it is not one
inline std::string crashVersion(const std::string& text) {
    static const std::string head = "OnAir ";
    if (text.compare(0, head.size(), head) != 0) return {};
    const size_t end = text.find(" crashed", head.size());
    const size_t nl = text.find('\n');
    if (end == std::string::npos || (nl != std::string::npos && end > nl)) return {};
    return text.substr(head.size(), end - head.size());
}

inline std::string crashFileName(const std::string& text, const std::string& running) {
    const std::string v = crashVersion(text);
    if (v == running) return "crash.txt";
    std::string safe;
    for (char c : v.empty() ? std::string("unknown version") : v) safe += (std::isalnum((unsigned char)c) || c == '.' || c == '-') ? c : '_';
    return "crash-from-" + safe + ".txt";
}

} // namespace bugreport
