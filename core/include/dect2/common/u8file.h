// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Opening a file at a UTF-8 path, which is what the file dialogs return. On Windows a std::string given to std::ofstream or
// fopen() is read in the ANSI code page, so a folder such as "OneDrive\Área de Trabalho" or a user name with an accent is not
// found and the file is not written.
#pragma once
#include <cstdio>
#include <filesystem>
#include <string>

namespace dect2 {

// UTF-8 with every byte that is not part of a valid sequence replaced by '_'. A service name from the air can be cut in the middle
// of a character or be in another character set; std::filesystem throws "Illegal byte sequence" (abort) on such a path on Windows.
inline std::string u8valid(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        size_t n = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        bool ok = n && i + n <= s.size();
        for (size_t k = 1; ok && k < n; k++) ok = ((unsigned char)s[i + k] & 0xC0) == 0x80;
        if (ok && n == 3) { const unsigned char c1 = (unsigned char)s[i + 1]; ok = !(c == 0xE0 && c1 < 0xA0) && !(c == 0xED && c1 >= 0xA0); }
        if (ok && n == 4) { const unsigned char c1 = (unsigned char)s[i + 1]; ok = !(c == 0xF0 && c1 < 0x90) && !(c == 0xF4 && c1 >= 0x90); }
        if (ok) { o.append(s, i, n); i += n; } else { o += '_'; i++; }
    }
    return o;
}

inline std::filesystem::path u8file(const std::string& p) {
    const std::string v = u8valid(p);
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(v.data()), v.size()));
}

// fopen() for a UTF-8 path; mode is plain ASCII ("wb", "rb")
inline FILE* u8fopen(const std::string& p, const char* mode) {
#ifdef _WIN32
    std::wstring m;
    for (const char* c = mode; *c; c++) m += (wchar_t)*c;
    return _wfopen(u8file(p).c_str(), m.c_str());
#else
    return fopen(p.c_str(), mode);
#endif
}

} // namespace dect2
