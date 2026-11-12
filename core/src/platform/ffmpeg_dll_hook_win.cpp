// Windows: where the FFmpeg DLLs come from. They are delay-loaded (cmake/FfmpegDelayLoad.cmake): on the first FFmpeg call the delay-load
// helper of mingw-w64 asks this hook first, and the hook loads avcodec-62.dll ... by full path from lib\ffmpeg in the program's folder
// (the installation folder: OnAir.exe, lib\ffmpeg\, lib\SoapySDR\ ...). A DLL that FFmpeg itself imports (avformat -> avcodec -> avutil)
// is found in that same folder first, so no other FFmpeg on the PATH can be mixed in.
// When lib\ffmpeg does not exist (a developer build run from elsewhere), the normal DLL search is used, as for an ordinary import.
// When it exists but a DLL in it does not load, the program stops with a message: running on another FFmpeg is never right.
// Compiled into every program that links FFmpeg (an INTERFACE source of the FFmpeg target), never into a library.
#include <windows.h>
#include <delayimp.h>
#include <array>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

bool startsWithNoCase(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != prefix[i]) return false;
    }
    return true;
}

bool isFfmpegDll(std::string_view name) {
    static constexpr std::array<std::string_view, 6> kNames = {"avutil-", "swresample-", "swscale-", "avcodec-", "avformat-", "avdevice-"};
    for (std::string_view p : kNames)
        if (startsWithNoCase(name, p)) return true;
    return false;
}

// the folder of the running program, with a trailing backslash
std::wstring programDir() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return std::wstring();
        if (n < path.size()) { path.resize(n); break; }
        path.resize(path.size() * 2);
    }
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
}

bool isFolder(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

[[noreturn]] void cannotLoad(const std::wstring& path) {
    std::wstring msg = L"OnAir cannot load the FFmpeg library\n" + path +
                       L"\n\nThe installation is incomplete or damaged: install OnAir again.";
    std::fputs("OnAir: cannot load an FFmpeg DLL from lib\\ffmpeg (incomplete installation)\n", stderr);
    FatalAppExitW(0, msg.c_str());
    ExitProcess(1);
}

FARPROC WINAPI ffmpegDllHook(unsigned notify, PDelayLoadInfo info) {
    if (notify != dliNotePreLoadLibrary || info == nullptr || info->szDll == nullptr) return nullptr;
    std::string_view name(info->szDll);
    if (!isFfmpegDll(name)) return nullptr;
    std::wstring dir = programDir() + L"lib\\ffmpeg\\";
    if (!isFolder(dir)) return nullptr;   // no lib\ffmpeg: the normal DLL search (a build folder without it)
    std::wstring path = dir + std::wstring(name.begin(), name.end());   // DLL names are ASCII
    // what this DLL imports: lib\ffmpeg itself, then the program's folder (a library FFmpeg needs is shipped there), then Windows
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    // a developer build (MSYS2's FFmpeg): the libraries it needs are found on the PATH; the FFmpeg DLL is still this one
    if (m == nullptr) m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (m == nullptr) cannotLoad(path);
    return reinterpret_cast<FARPROC>(m);
}

}  // namespace

// read by mingw-w64's __delayLoadHelper2; this definition replaces the empty one in libmingwex
extern "C" PfnDliHook __pfnDliNotifyHook2 = ffmpegDllHook;
