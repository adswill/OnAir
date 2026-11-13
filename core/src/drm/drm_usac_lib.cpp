// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Run-time loader for the optional libfdk-aac USAC decoder (see drm_usac_lib.h). Not shipped, not linked.
#include "dect2/drm/drm_usac_lib.h"
#include "../source/native_common.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace dect2 { namespace drm {

UsacLib UsacLib::fromResolver(const std::function<void*(const char*)>& sym) {
    UsacLib l;
    l.open = (OpenFn)sym("aacDecoder_Open"); l.configRaw = (ConfigRawFn)sym("aacDecoder_ConfigRaw"); l.fill = (FillFn)sym("aacDecoder_Fill");
    l.decode = (DecodeFn)sym("aacDecoder_DecodeFrame"); l.info = (InfoFn)sym("aacDecoder_GetStreamInfo"); l.close = (CloseFn)sym("aacDecoder_Close");
    return l;
}

const char* UsacLib::libraryHint() { return "libfdk-aac (libfdk-aac.so.2, libfdk-aac.2.dylib or libfdk-aac-2.dll)"; }

// The libfdk-aac files in the OnAir codecs folders (docs/CODECS.md): the usual names first, then any file with "fdk" and "aac" in its
// name and a library extension, so a differently named build (fdk-aac-2.dll, libfdk-aac.2.2.0.dylib) is found too.
std::vector<std::string> UsacLib::codecFolderCandidates() {
    std::vector<std::string> v;
    for (const std::string& dir : native::codecFolders()) {
        for (const char* n : {"libfdk-aac-2.dll", "libfdk-aac.dll", "fdk-aac.dll", "libfdk-aac-1.dll", "fdk-aac-2.dll", "libfdk-aac.so.2", "libfdk-aac.so",
                              "libfdk-aac.2.dylib", "libfdk-aac.dylib"}) v.push_back(dir + n);
        std::error_code ec;
        std::vector<std::string> extra;
        try {   // a folder name the system cannot convert must not end the program (std::filesystem throws on it, #329)
        for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(dir), ec)) {
            const std::string f = e.path().filename().string();
            std::string low = f;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (low.find("fdk") == std::string::npos || low.find("aac") == std::string::npos) continue;
            if (low.find(".dll") == std::string::npos && low.find(".dylib") == std::string::npos && low.find(".so") == std::string::npos) continue;
            if (std::find(v.begin(), v.end(), dir + f) == v.end()) extra.push_back(dir + f);
        }
        } catch (const std::exception&) {}
        std::sort(extra.begin(), extra.end());
        v.insert(v.end(), extra.begin(), extra.end());
    }
    return v;
}

namespace {
std::mutex gRepMu;
UsacLib::Report gRep;
std::atomic<uint64_t> gGen{1};
bool fileThere(const std::string& p) { try { std::error_code ec; return std::filesystem::is_regular_file(std::filesystem::path(p), ec); } catch (const std::exception&) { return false; } }
#ifdef _WIN32
// why LoadLibrary refused a file that is there, in plain words (#266: the DLL was in the codecs folder but nothing said why it did not load)
std::string winLoadError(DWORD e) {
    switch (e) {
    case ERROR_MOD_NOT_FOUND: return "a DLL it needs is missing (put the DLLs that came with it, e.g. libgcc_s_seh-1.dll, libwinpthread-1.dll, libstdc++-6.dll, in the same folder)";
    case ERROR_BAD_EXE_FORMAT: return "wrong kind of DLL (a 32-bit build: OnAir needs the 64-bit libfdk-aac)";
    case ERROR_ACCESS_DENIED: return "access denied (blocked by the antivirus or the file's permissions)";
    default: return "Windows error " + std::to_string((unsigned long)e);
    }
}
#endif
} // namespace

UsacLib UsacLib::load(const std::string& path) {
    std::vector<std::string> names;
    if (!path.empty()) names.push_back(path);
    if (const char* e = std::getenv("DECT2_FDKAAC_LIB")) if (*e) names.push_back(e);
    for (const std::string& n : codecFolderCandidates()) names.push_back(n);   // a copy in a codecs folder wins over the system's
    void* h = nullptr;
    Report rep;
    std::string used;
#ifdef _WIN32
    for (const char* n : {"libfdk-aac-2.dll", "libfdk-aac.dll", "fdk-aac.dll", "libfdk-aac-1.dll"}) names.push_back(n);
    for (const std::string& n : names) {
        const bool full = n.find('\\') != std::string::npos || n.find('/') != std::string::npos;   // a full path: its own folder is searched for the DLLs it needs
        if (full && !fileThere(n)) continue;
        std::wstring wn;   // the paths are UTF-8: the wide call finds a file in a folder with non-ASCII letters
        if (const int wl = MultiByteToWideChar(CP_UTF8, 0, n.c_str(), -1, nullptr, 0); wl > 1) { wn.resize((size_t)wl - 1); MultiByteToWideChar(CP_UTF8, 0, n.c_str(), -1, wn.data(), wl); }
        if (wn.empty()) continue;
        if ((h = full ? (void*)LoadLibraryExW(wn.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) : (void*)LoadLibraryW(wn.c_str()))) { used = n; break; }
        if (full) rep.failed.push_back(n + ": " + winLoadError(GetLastError()));
    }
    auto sym = [&](const char* s) -> void* { return h ? (void*)GetProcAddress((HMODULE)h, s) : nullptr; };
#else
    for (const char* n : {"libfdk-aac.so.2", "libfdk-aac.so", "libfdk-aac.2.dylib", "libfdk-aac.dylib", "/opt/homebrew/lib/libfdk-aac.dylib", "/usr/local/lib/libfdk-aac.dylib"}) names.push_back(n);
    for (const std::string& n : names) {
        const bool full = n.find('/') != std::string::npos;
        if (full && !fileThere(n)) continue;
        if ((h = dlopen(n.c_str(), RTLD_NOW | RTLD_LOCAL))) { used = n; break; }
        if (full) { const char* why = dlerror(); rep.failed.push_back(n + ": " + (why ? why : "cannot be loaded")); }
    }
    auto sym = [&](const char* s) -> void* { return h ? dlsym(h, s) : nullptr; };
#endif
    UsacLib l = fromResolver(sym);   // the library handle stays open for the life of the program
    if (h && !l.ok()) rep.failed.push_back(used + ": loaded, but it has no AAC decoder functions (not libfdk-aac, or a build without the decoder)");
    rep.loaded = l.ok();
    if (rep.loaded) rep.path = used;
    { std::lock_guard<std::mutex> lk(gRepMu); gRep = rep; }
    return l;
}

UsacLib::Report UsacLib::lastReport() { std::lock_guard<std::mutex> lk(gRepMu); return gRep; }

namespace {
std::mutex gSharedMu;
UsacLib gShared;
uint64_t gSharedGen = 0;
}
UsacLib UsacLib::shared() {
    std::lock_guard<std::mutex> lk(gSharedMu);
    const uint64_t g = gGen.load();
    if (gSharedGen != g) { gShared = load(); gSharedGen = g; }
    return gShared;
}
void UsacLib::rescan() { gGen++; }
void UsacLib::setSharedForTest(const UsacLib& l) { std::lock_guard<std::mutex> lk(gSharedMu); gShared = l; gSharedGen = gGen.load(); }
uint64_t UsacLib::generation() { return gGen.load(); }

bool UsacSession::open(const std::vector<uint8_t>& asc) {
    close();
    err_ = 0; step_ = "";
    if (!lib_.ok() || asc.empty()) return false;
    h_ = lib_.open(0 /* TT_MP4_RAW */, 1);
    if (!h_) { step_ = "aacDecoder_Open"; err_ = -1; return false; }
    uint8_t* conf[1] = {const_cast<uint8_t*>(asc.data())};
    const unsigned len[1] = {(unsigned)asc.size()};
    if (const int e = lib_.configRaw(h_, conf, len); e != 0) { step_ = "aacDecoder_ConfigRaw"; err_ = e; close(); return false; }
    return true;
}

void UsacSession::close() { if (h_ && lib_.close) lib_.close(h_); h_ = nullptr; }

bool UsacSession::decode(const std::vector<uint8_t>& au, std::vector<int16_t>& pcm, int& rate, int& channels) {
    pcm.clear();
    err_ = 0; step_ = "";
    if (!h_ || au.empty()) return false;
    uint8_t* buf[1] = {const_cast<uint8_t*>(au.data())};
    const unsigned size[1] = {(unsigned)au.size()};
    unsigned valid = (unsigned)au.size();
    if (const int e = lib_.fill(h_, buf, size, &valid); e != 0) { step_ = "aacDecoder_Fill"; err_ = e; return false; }
    std::vector<int16_t> tmp(8 * 4096);
    // 0x4000 .. 0x4FFF are libfdk-aac's decode errors: it has concealed the frame and the output is valid (IS_OUTPUT_VALID in aacdecoder_lib.h).
    // Throwing that output away left a 64 ms hole for every such frame (#331, #332).
    if (const int e = lib_.decode(h_, tmp.data(), (int)tmp.size(), 0); e != 0) {
        step_ = "aacDecoder_DecodeFrame"; err_ = e;
        if (e < 0x4000 || e > 0x4FFF) return false;
    }
    const int* si = lib_.info(h_);
    if (!si) { step_ = "aacDecoder_GetStreamInfo"; err_ = -1; return false; }
    rate = si[0]; channels = si[2];
    const int frame = si[1];
    if (rate <= 0 || channels <= 0 || frame <= 0 || (size_t)frame * (size_t)channels > tmp.size()) return false;
    pcm.assign(tmp.begin(), tmp.begin() + (size_t)frame * (size_t)channels);
    return true;
}

}} // namespace dect2::drm
