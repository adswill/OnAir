// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "recdir.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>

namespace fs = std::filesystem;
namespace {
fs::path P(const std::string& u8) { return fs::path(reinterpret_cast<const char8_t*>(u8.c_str())); }
bool writable(const std::string& dir) {
    std::error_code ec;
    fs::create_directories(P(dir), ec);
    if (!fs::is_directory(P(dir), ec)) return false;
    const fs::path t = P(dir) / ".onair_write_test";
    { std::ofstream f(t, std::ios::binary); if (!f) return false; f << 'x'; if (!f) return false; }
    fs::remove(t, ec);
    return true;
}
}  // namespace

namespace recdir {

std::string normalise(std::string p) {
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) {
        if (p.size() == 3 && p[1] == ':') break;   // "C:\"
        p.pop_back();
    }
    return p;
}

std::string resolve(const std::string& chosen, const std::string& defaultDir, bool* fellBack) {
    if (fellBack) *fellBack = false;
    const std::string c = normalise(chosen);
    const std::string d = normalise(defaultDir);
    if (!c.empty()) {
        if (writable(c)) return c;
        if (fellBack) *fellBack = true;
    }
    writable(d);
    return d;
}

unsigned long long freeBytes(const std::string& dir) {
    std::error_code ec;
    const auto s = fs::space(P(dir), ec);
    return ec ? UINT64_MAX : (unsigned long long)s.available;
}

}  // namespace recdir

