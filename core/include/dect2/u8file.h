// Opening a file at a UTF-8 path, which is what the file dialogs return. On Windows a std::string given to std::ofstream or
// fopen() is read in the ANSI code page, so a folder such as "OneDrive\Área de Trabalho" or a user name with an accent is not
// found and the file is not written.
#pragma once
#include <cstdio>
#include <filesystem>
#include <string>

namespace dect2 {

inline std::filesystem::path u8file(const std::string& p) { return std::filesystem::path(reinterpret_cast<const char8_t*>(p.c_str())); }

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
