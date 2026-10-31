#include "dect2/codec/codec_paths.h"
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

namespace dect2::codec {

namespace {

bool windowsStyle(const std::string& p) {
    if (p.find('\\') != std::string::npos) return true;
    return p.size() >= 2 && std::isalpha((unsigned char)p[0]) && p[1] == ':';
}

std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

std::mutex gMu;
std::string gExtra;

} // namespace

std::string normalizeDirPath(const std::string& in) {
    if (in.empty()) return {};
    const bool win = windowsStyle(in);
    const char sep = win ? '\\' : '/';
    std::string s = in;
    for (char& c : s) if (c == '\\') c = '/';
    std::string prefix;    // "//" (network share), "C:" or "/"
    bool rooted = false;
    size_t i = 0;
    if (win && s.size() >= 2 && s[0] == '/' && s[1] == '/') { prefix = "//"; rooted = true; i = 2; }
    else if (s.size() >= 2 && std::isalpha((unsigned char)s[0]) && s[1] == ':') {
        prefix = s.substr(0, 2);
        i = 2;
        if (i < s.size() && s[i] == '/') { rooted = true; ++i; }
    } else if (s[0] == '/') { prefix = "/"; rooted = true; i = 1; }
    std::vector<std::string> parts;
    for (size_t j = i; j <= s.size();) {
        size_t k = s.find('/', j);
        if (k == std::string::npos) k = s.size();
        const std::string seg = s.substr(j, k - j);
        if (seg.empty() || seg == ".") {}
        else if (seg == "..") { if (!parts.empty() && parts.back() != "..") parts.pop_back(); else if (!rooted) parts.push_back(seg); }
        else parts.push_back(seg);
        j = k + 1;
    }
    std::string out = prefix;
    if (prefix.size() == 2 && prefix[1] == ':' && rooted) out += '/';
    for (size_t n = 0; n < parts.size(); ++n) { if (n) out += '/'; out += parts[n]; }
    for (char& c : out) if (c == '/') c = sep;
    return out;
}

bool samePath(const std::string& a, const std::string& b) {
    const std::string na = normalizeDirPath(a), nb = normalizeDirPath(b);
    if (windowsStyle(a) || windowsStyle(b)) return lower(na) == lower(nb);
    return na == nb;
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    const char sep = windowsStyle(dir) ? '\\' : '/';
    if (dir.back() == '/' || dir.back() == '\\') return dir + name;
    return dir + sep + name;
}

std::vector<CodecDir> codecSearchDirs(const CodecPathInput& in) {
    std::vector<CodecDir> v;
    auto add = [&](const std::string& path, const char* source) {
        if (path.empty()) return;
        const std::string n = normalizeDirPath(path);
        if (n.empty()) return;
        for (const auto& d : v) if (samePath(d.path, n)) return;
        v.push_back({n, source});
    };
    add(in.userDir, "user");
    add(in.extraDir, "extra");
    add(in.envDir, "ONAIR_CODEC_DIR");
    if (!in.exeDir.empty()) {
        const std::string exe = normalizeDirPath(in.exeDir);
        const char sep = windowsStyle(exe) ? '\\' : '/';
        add(exe + sep + "codecs", "program");
        // macOS: <folder>/OnAir.app/Contents/MacOS is the program; a codecs folder beside the app and one inside it (Resources) count too
        const std::string tail = std::string(".app") + sep + "Contents" + sep + "MacOS";
        if (exe.size() > tail.size() && exe.compare(exe.size() - tail.size(), tail.size(), tail) == 0) {
            const std::string app = exe.substr(0, exe.size() - tail.size() + 4);    // .../OnAir.app
            const size_t up = app.find_last_of("/\\");
            if (up != std::string::npos && up > 0) add(app.substr(0, up) + sep + "codecs", "program");
            add(app + sep + "Contents" + sep + "Resources" + sep + "codecs", "program");
        }
    }
    return v;
}

std::vector<CodecDir> dirsWithFile(const std::vector<CodecDir>& dirs, const std::string& name, const std::function<bool(const std::string&)>& exists) {
    std::vector<CodecDir> r;
    for (const auto& d : dirs) if (exists(joinPath(d.path, name))) r.push_back(d);
    return r;
}

std::vector<std::string> libraryPaths(const std::vector<CodecDir>& dirs, const std::vector<std::string>& extraDirs, const std::vector<std::string>& names,
                                      const std::function<bool(const std::string&)>& exists) {
    std::vector<std::string> folders, out;
    for (const auto& d : dirs) folders.push_back(d.path);
    for (const auto& d : extraDirs) if (!d.empty()) folders.push_back(d);
    std::vector<std::string> seen;
    for (const auto& f : folders) {
        bool dup = false;
        for (const auto& s : seen) dup |= samePath(s, f);
        if (dup) continue;
        seen.push_back(f);
        for (const auto& n : names) {
            const std::string full = joinPath(f, n);
            if (exists(full)) out.push_back(full);
        }
    }
    return out;
}

void setExtraCodecDir(const std::string& dir) { std::lock_guard<std::mutex> lk(gMu); gExtra = dir; }
std::string extraCodecDir() { std::lock_guard<std::mutex> lk(gMu); return gExtra; }

#if defined(_WIN32)
// Windows ANSI calls return the system code page (GBK, 1251, ...), not UTF-8; std::filesystem then threw "Cannot convert character sequence" on a
// user name or install folder with non-ASCII letters (the DRM crash of #329). The wide calls, converted to UTF-8, are what the rest of the code expects.
static std::string wideToUtf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
#endif

std::string programDir() {
    std::string exe;
#if defined(_WIN32)
    wchar_t path[MAX_PATH * 2] = {0};
    if (GetModuleFileNameW(nullptr, path, (DWORD)(sizeof path / sizeof *path) - 1)) exe = wideToUtf8(path);
#elif defined(__APPLE__)
    char path[4096];
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size) == 0) exe = path;
#else
    char path[4096];
    const ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n > 0) exe.assign(path, (size_t)n);
#endif
    const size_t cut = exe.find_last_of("/\\");
    return cut == std::string::npos ? std::string() : exe.substr(0, cut);
}

std::string userCodecDir() {
    if (const char* o = getenv("DECT2_DATA_DIR"); o && *o) return joinPath(o, "codecs");
#if defined(_WIN32)
    if (const wchar_t* la = _wgetenv(L"LOCALAPPDATA"); la && *la) return wideToUtf8(la) + "\\OnAir\\codecs";
    return {};
#else
    const char* home = getenv("HOME");
#if defined(__APPLE__)
    if (home && *home) return std::string(home) + "/Library/Application Support/OnAir/codecs";
    return {};
#else
    const char* x = getenv("XDG_DATA_HOME");
    if (x && *x) return std::string(x) + "/onair/codecs";
    if (home && *home) return std::string(home) + "/.local/share/onair/codecs";
    return {};
#endif
#endif
}

std::vector<CodecDir> systemCodecDirs() {
    CodecPathInput in;
    in.exeDir = programDir();
    in.userDir = userCodecDir();
    if (const char* e = getenv("ONAIR_CODEC_DIR")) in.envDir = e;
    in.extraDir = extraCodecDir();
    return codecSearchDirs(in);
}

std::vector<std::string> systemLibraryPaths(const std::vector<std::string>& names) {
    const auto exists = [](const std::string& p) {
        try {
            const std::u8string u(reinterpret_cast<const char8_t*>(p.data()), p.size());   // the paths are UTF-8, also on Windows
            std::error_code ec;
            return std::filesystem::is_regular_file(std::filesystem::path(u), ec);
        } catch (const std::exception&) { return false; }
    };
    return libraryPaths(systemCodecDirs(), {programDir()}, names, exists);
}

} // namespace dect2::codec
