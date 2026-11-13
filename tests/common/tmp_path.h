// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A path inside the system temp folder (a native Windows program has no /tmp).
#pragma once
#include <filesystem>
#include <string>
#include <utility>

// Converts to both std::string and const char*; keep it in a named variable when a const char* is needed for longer than one statement.
struct TmpPath : std::string {
    TmpPath(std::string s) : std::string(std::move(s)) {}
    operator const char*() const { return c_str(); }
};

inline TmpPath tmpPath(const std::string& name) {
    std::error_code ec;
    std::filesystem::path d = std::filesystem::temp_directory_path(ec);
    if (ec) d = ".";
    return TmpPath((d / name).string());
}
