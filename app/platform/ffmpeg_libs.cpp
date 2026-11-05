// See ffmpeg_libs.h.
#include "ffmpeg_libs.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/ffversion.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#if defined(__linux__)
#include <elf.h>
#include <link.h>
#endif
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace ffmpeglibs {
namespace fs = std::filesystem;

namespace {

Status gStatus;

// the libraries in load order (each needs only the ones before it), with the version of the headers this program was built with
struct Lib { const char* name; int major; unsigned built; };
const Lib kLibs[] = {
    {"avutil", LIBAVUTIL_VERSION_MAJOR, LIBAVUTIL_VERSION_INT}, {"swresample", LIBSWRESAMPLE_VERSION_MAJOR, LIBSWRESAMPLE_VERSION_INT},
    {"swscale", LIBSWSCALE_VERSION_MAJOR, LIBSWSCALE_VERSION_INT}, {"avcodec", LIBAVCODEC_VERSION_MAJOR, LIBAVCODEC_VERSION_INT},
    {"avformat", LIBAVFORMAT_VERSION_MAJOR, LIBAVFORMAT_VERSION_INT},
};

// the check copy of the program (check()): its folder is in this variable, and it ends with one of these codes
const char* const kCheckVar = "ONAIR_FFMPEG_CHECK";
enum : int { kOk = 0, kBadVersion = 3, kElsewhere = 4, kLoadFailed = 5 };
const int kCheckSeconds = 20;

std::string fileName(const Lib& l) {
    const std::string m = std::to_string(l.major);
#if defined(_WIN32)
    return std::string(l.name) + "-" + m + ".dll";
#elif defined(__APPLE__)
    return "lib" + std::string(l.name) + "." + m + ".dylib";
#else
    return "lib" + std::string(l.name) + ".so." + m;
#endif
}

fs::path u8path(const std::string& s) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    return fs::u8path(s);
#endif
}
std::string u8str(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string verStr(unsigned v) { return std::to_string(v >> 16) + "." + std::to_string((v >> 8) & 255) + "." + std::to_string(v & 255); }

// "" when the library's version fits the one OnAir was built with: the same major version, no older minor version
std::string versionProblem(const Lib& l, unsigned v) {
    if ((int)(v >> 16) != l.major) return "lib" + std::string(l.name) + " " + verStr(v) + " has another major version than OnAir's " + verStr(l.built) + ".";
    if (v < l.built) return "lib" + std::string(l.name) + " " + verStr(v) + " is older than OnAir's " + verStr(l.built) + ".";
    return "";
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    size_t a = 0;
    while (a < s.size() && (s[a] == '\n' || s[a] == '\r' || s[a] == ' ')) a++;
    s = s.substr(a);
    if (s.size() > 500) { s.resize(500); s += "..."; }
    return s;
}

bool sameFolder(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    const bool eq = fs::equivalent(a, b, ec);
    return !ec && eq;
}

// the majors of the libavcodec files in dir, lib/ and bin/ (another FFmpeg version has other file names)
std::vector<int> avcodecMajors(const fs::path& base) {
    std::vector<int> v;
    std::error_code ec;
    for (const fs::path& d : {base, base / "lib", base / "bin"}) {
        if (!fs::is_directory(d, ec)) continue;
        for (const auto& e : fs::directory_iterator(d, ec)) {
            const std::string n = u8str(e.path().filename());
            int m = -1;
#if defined(_WIN32)
            if (sscanf(n.c_str(), "avcodec-%d.dll", &m) != 1) m = -1;
#elif defined(__APPLE__)
            if (sscanf(n.c_str(), "libavcodec.%d.dylib", &m) != 1) m = -1;
#else
            if (sscanf(n.c_str(), "libavcodec.so.%d", &m) != 1) m = -1;
#endif
            if (m >= 0) v.push_back(m);
        }
    }
    return v;
}

std::string selfExe() {
#if defined(_WIN32)
    wchar_t p[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, p, (DWORD)(sizeof p / sizeof p[0]));
    return n ? u8str(fs::path(std::wstring(p, n))) : std::string();
#elif defined(__APPLE__)
    uint32_t n = 0;
    _NSGetExecutablePath(nullptr, &n);
    std::string p(n, '\0');
    if (_NSGetExecutablePath(p.data(), &n) != 0) return "";
    if (const size_t z = p.find('\0'); z != std::string::npos) p.resize(z);
    std::error_code ec;
    const fs::path c = fs::canonical(p, ec);
    return ec ? p : c.string();
#else
    std::error_code ec;
    const fs::path c = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : c.string();
#endif
}

#ifdef _WIN32
std::string winError(DWORD e) {
    LPSTR m = nullptr;   // FORMAT_MESSAGE_ALLOCATE_BUFFER: Windows allocates the text and sets m; len is its length (0: no text)
    const DWORD len = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                                     reinterpret_cast<LPSTR>(&m), 0, nullptr);
    std::string s;
    if (len > 0) {
        s = trim(std::string(m, len));
        LocalFree(m);
    }
    return "Windows error " + std::to_string((unsigned long)e) + (s.empty() ? "" : ": " + s);
}
// loads the libraries of folder by their full path, in dependency order (each one's own imports are looked up in its folder first) and
// checks their versions; the delay-load stubs of the program then find the modules already loaded under these names. kOk, or the
// code and what is wrong (then they are unloaded again)
int winLoad(const std::string& folder, std::string& problem) {
    std::vector<HMODULE> loaded;
    auto undo = [&] { for (auto it = loaded.rbegin(); it != loaded.rend(); ++it) FreeLibrary(*it); };
    for (const Lib& l : kLibs) {
        const fs::path p = u8path(folder) / fileName(l);
        HMODULE h = LoadLibraryExW(p.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!h) { problem = fileName(l) + " could not be loaded (" + winError(GetLastError()) + ")."; undo(); return kLoadFailed; }
        loaded.push_back(h);
        auto ver = (unsigned (*)(void))(void*)GetProcAddress(h, (std::string(l.name) + "_version").c_str());
        if (!ver) { problem = fileName(l) + " has no " + l.name + "_version: it is not an FFmpeg library."; undo(); return kBadVersion; }
        const std::string vp = versionProblem(l, ver());
        if (!vp.empty()) { problem = vp; undo(); return kBadVersion; }
    }
    return kOk;
}
#else
const char* const kLoaderVar =
#ifdef __APPLE__
    "DYLD_LIBRARY_PATH";
#else
    "LD_LIBRARY_PATH";
#endif
// set in the restarted program: the folder it was started for, and the loader path it had before (put back so programs it starts do not inherit the FFmpeg folder)
const char* const kActiveVar = "ONAIR_FFMPEG_ACTIVE";
const char* const kOldPathVar = "ONAIR_FFMPEG_OLD_LIBPATH";

std::string loaderPath(const std::string& folder) {
    const char* old = getenv(kLoaderVar);
    return old && *old ? folder + ":" + old : folder;
}
#endif

// the check copy: FFmpeg has been loaded (Linux, macOS: by the system loader from the loader path; Windows: here). Never returns.
[[noreturn]] void checkCopy(const std::string& folder) {
#ifdef _WIN32
    std::string problem;
    const int r = winLoad(folder, problem);
    if (r != kOk) { fprintf(stderr, "%s\n", problem.c_str()); fflush(stderr); _exit(r); }
#else
    unsigned (*const ver[])(void) = {avutil_version, swresample_version, swscale_version, avcodec_version, avformat_version};
    for (size_t i = 0; i < sizeof kLibs / sizeof kLibs[0]; i++) {
        const std::string vp = versionProblem(kLibs[i], ver[i]());
        if (!vp.empty()) { fprintf(stderr, "%s\n", vp.c_str()); fflush(stderr); _exit(kBadVersion); }
        Dl_info d{};
        if (dladdr((const void*)ver[i], &d) && d.dli_fname && !sameFolder(fs::path(d.dli_fname).parent_path(), folder)) {
            fprintf(stderr, "lib%s was taken from %s\n", kLibs[i].name, d.dli_fname); fflush(stderr); _exit(kElsewhere);
        }
    }
#endif
    _exit(kOk);
}

// runs the check copy of this program for folder: its exit code (-1: it could not be started) and what it printed
int runCheckCopy(const std::string& folder, std::string& out) {
    const std::string exe = selfExe();
    if (exe.empty()) { out = "the program's own path is unknown"; return -1; }
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) { out = winError(GetLastError()); return -1; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + u8path(exe).wstring() + L"\"";
    SetEnvironmentVariableW(L"ONAIR_FFMPEG_CHECK", u8path(folder).wstring().c_str());   // the copy inherits it
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    const DWORD startErr = GetLastError();
    SetEnvironmentVariableW(L"ONAIR_FFMPEG_CHECK", nullptr);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); out = winError(startErr); return -1; }
    if (WaitForSingleObject(pi.hProcess, kCheckSeconds * 1000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    char b[4096];
    DWORD n = 0, avail = 0;
    while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail && ReadFile(rd, b, sizeof b, &n, nullptr) && n) out.append(b, n);
    CloseHandle(rd); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    out = trim(out);
    return (int)code;
#else
    // the environment of the copy, made before fork (a forked copy of a program with threads may only call async-signal-safe functions)
    std::vector<std::string> env;
    for (char** e = environ; e && *e; e++) {
        const std::string s = *e;
        if (s.rfind(std::string(kLoaderVar) + "=", 0) == 0 || s.rfind(std::string(kCheckVar) + "=", 0) == 0 || s.rfind(std::string(kActiveVar) + "=", 0) == 0) continue;
        env.push_back(s);
    }
    env.push_back(std::string(kLoaderVar) + "=" + loaderPath(folder));
    env.push_back(std::string(kCheckVar) + "=" + folder);
    std::vector<char*> envp;
    for (std::string& s : env) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::string exeCopy = exe;
    char* argv[] = {exeCopy.data(), nullptr};
    int fd[2];
    if (pipe(fd) != 0) { out = strerror(errno); return -1; }
    const pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); out = strerror(errno); return -1; }
    if (pid == 0) {
        dup2(fd[1], 1); dup2(fd[1], 2);
        close(fd[0]); close(fd[1]);
        execve(exe.c_str(), argv, envp.data());
        _exit(126);
    }
    close(fd[1]);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(kCheckSeconds);
    char b[4096];
    for (;;) {
        const int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
        if (left <= 0) { kill(pid, SIGKILL); break; }
        pollfd p{fd[0], POLLIN, 0};
        const int r = poll(&p, 1, left);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) continue;
        const ssize_t n = read(fd[0], b, sizeof b);   // Flawfinder: ignore (at most sizeof b bytes, only n of them are used)
        if (n <= 0) break;   // the copy has ended (its end of the pipe is closed)
        out.append(b, (size_t)n);
    }
    close(fd[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    out = trim(out);
    // the loader's message starts with the program's path ("/path/dect2: error while loading shared libraries: libx.so.1: ..."): keep what matters
    const std::string lead = "error while loading shared libraries: ";
    if (const size_t k = out.find(lead); k != std::string::npos) out = out.substr(k + lead.size());
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return -1;
#endif
}

}   // namespace

std::vector<std::string> libFileNames() {
    std::vector<std::string> v;
    for (const Lib& l : kLibs) v.push_back(fileName(l));
    return v;
}

std::string findLibFolder(const std::string& dir) {
    if (dir.empty()) return "";
    std::error_code ec;
    const fs::path base = u8path(dir);
    for (const fs::path& d : {base, base / "lib", base / "bin"}) {
        if (!fs::is_directory(d, ec)) continue;
        bool all = true;
        for (const Lib& l : kLibs) all = all && fs::exists(d / fileName(l), ec);
        if (all) return u8str(d);
    }
    return "";
}

Check check(const std::string& dir) {
    Check c;
    const std::string onair = std::string("OnAir is built with FFmpeg ") + FFMPEG_VERSION + ".";
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(u8path(dir), ec)) {
        c.problem = "The folder " + dir + " does not exist.";
        c.solution = "Choose the folder of an FFmpeg installation.";
        return c;
    }
    c.folder = findLibFolder(dir);
    if (c.folder.empty()) {
        const std::vector<int> other = avcodecMajors(u8path(dir));
        if (!other.empty()) {
            c.problem = dir + " holds an FFmpeg with libavcodec " + std::to_string(other[0]) + "; OnAir needs libavcodec " + std::to_string(LIBAVCODEC_VERSION_MAJOR) + ". " + onair;
            c.solution = "Install a shared FFmpeg build with the same major version as OnAir's (FFmpeg " + std::string(FFMPEG_VERSION) + " or a newer one with libavcodec " + std::to_string(LIBAVCODEC_VERSION_MAJOR) + ") and choose its folder.";
        } else {
            std::string names;
            for (const std::string& n : libFileNames()) names += (names.empty() ? "" : ", ") + n;
            c.problem = "No FFmpeg libraries in " + dir + " (OnAir needs " + names + ").";
            c.solution = "Choose the folder that holds the libraries of a shared FFmpeg build (bin on Windows, lib on Linux and macOS). A build with only the ffmpeg program has no libraries.";
        }
        return c;
    }
    std::string out;
    const int r = runCheckCopy(c.folder, out);
    switch (r) {
    case kOk: c.ok = true; return c;
    case kBadVersion:
        c.problem = out + " " + onair;
        c.solution = "Use an FFmpeg build of a similar version: " + std::string(FFMPEG_VERSION) + " or a newer one with the same major versions (libavcodec " + std::to_string(LIBAVCODEC_VERSION_MAJOR) + ").";
        return c;
    case kElsewhere:
        c.problem = "Not all FFmpeg libraries came from " + c.folder + ": " + out + ".";
        c.solution = "Choose a folder that holds all five FFmpeg libraries together.";
        return c;
    case kLoadFailed:
    case 127:   // Linux, macOS: the system loader could not start the program with them, and says why
        c.problem = "The FFmpeg libraries in " + c.folder + " could not be loaded: " + (out.empty() ? "no reason given" : out);
        if (out.find("file too short") != std::string::npos || out.find("invalid ELF") != std::string::npos || out.find("wrong ELF class") != std::string::npos ||
            out.find("not a mach-o") != std::string::npos || out.find("incompatible architecture") != std::string::npos || out.find("%1") != std::string::npos ||
            out.find("not a valid Win32") != std::string::npos)
            c.solution = "The files are damaged or are not FFmpeg libraries for this system (64-bit " +
#if defined(_WIN32)
                std::string("Windows") +
#elif defined(__APPLE__)
                std::string("macOS") +
#else
                std::string("Linux") +
#endif
                "): install the FFmpeg build again, or use a build for this system.";
        else
            c.solution = "A library they need is missing (for example libx264, libx265 or a runtime library of that build): install it, or put it in the same folder, or use a build that brings its own.";
        return c;
    case -1:
        c.problem = "OnAir could not test the libraries: " + out + ".";
        c.solution = "Try again, or choose another FFmpeg installation.";
        return c;
    default:
        c.problem = "OnAir stopped (code " + std::to_string(r) + ") when it tried the FFmpeg libraries in " + c.folder + (out.empty() ? "." : ": " + out);
        c.solution = "This FFmpeg build does not work with OnAir: use another build of a similar version (FFmpeg " + std::string(FFMPEG_VERSION) + " or a newer 8.x).";
        return c;
    }
}

const Status& status() { return gStatus; }

void apply(const std::string& dir, int argc, char** argv) {
    (void)argc;
    if (const char* chk = getenv(kCheckVar)) checkCopy(chk);   // the check copy of check(): never returns
    gStatus = Status();
    gStatus.chosen = dir;
#ifndef _WIN32
    if (const char* act = getenv(kActiveVar)) {
        // this is the restarted program: the loader has taken the libraries from the folder already
        gStatus.folder = act;
        gStatus.external = true;
        if (gStatus.chosen.empty()) gStatus.chosen = act;
        if (const char* old = getenv(kOldPathVar)) { if (*old) setenv(kLoaderVar, old, 1); else unsetenv(kLoaderVar); }
        unsetenv(kOldPathVar);
        unsetenv(kActiveVar);
        return;
    }
#endif
    if (dir.empty()) return;
    // the same trial load as in Settings: a folder that is wrong never reaches the program, it starts with its own FFmpeg
    const Check c = check(dir);
    if (!c.ok) {
        gStatus.problem = c.problem;
        gStatus.solution = c.solution;
        fprintf(stderr, "OnAir: FFmpeg libraries: %s %s OnAir uses its own FFmpeg.\n", c.problem.c_str(), c.solution.c_str());
        return;
    }
#ifdef _WIN32
    std::string problem;
    if (winLoad(c.folder, problem) != kOk) {
        gStatus.problem = problem;
        gStatus.solution = "Choose another FFmpeg installation of a similar version.";
        fprintf(stderr, "OnAir: FFmpeg libraries: %s OnAir uses its own FFmpeg.\n", problem.c_str());
        return;
    }
    gStatus.folder = c.folder;
    gStatus.external = true;
    fprintf(stderr, "OnAir: FFmpeg libraries from %s\n", c.folder.c_str());
#else
    const std::string exe = selfExe();
    const char* old = getenv(kLoaderVar);
    setenv(kOldPathVar, old ? old : "", 1);
    setenv(kActiveVar, c.folder.c_str(), 1);
    setenv(kLoaderVar, loaderPath(c.folder).c_str(), 1);
    fprintf(stderr, "OnAir: restarting with the FFmpeg libraries of %s\n", c.folder.c_str());
    fflush(stderr);
    execv(exe.c_str(), argv);
    // only on failure
    gStatus.problem = std::string("OnAir could not restart with the FFmpeg libraries of ") + c.folder + ": " + strerror(errno) + ".";
    gStatus.solution = "Start OnAir again.";
    if (old) setenv(kLoaderVar, old, 1); else unsetenv(kLoaderVar);
    unsetenv(kActiveVar);
    unsetenv(kOldPathVar);
    fprintf(stderr, "OnAir: FFmpeg libraries: %s\n", gStatus.problem.c_str());
#endif
}

Info info() {
    Info i;
    i.version = av_version_info();
    std::string path;
#ifdef _WIN32
    (void)avcodec_version();   // binds the delay-loaded DLL
    if (HMODULE h = GetModuleHandleW((L"avcodec-" + std::to_wstring(LIBAVCODEC_VERSION_MAJOR) + L".dll").c_str())) {
        wchar_t p[MAX_PATH * 4];
        const DWORD n = GetModuleFileNameW(h, p, (DWORD)(sizeof p / sizeof p[0]));
        if (n) path = u8str(fs::path(std::wstring(p, n)));
    }
#else
    Dl_info d{};
    if (dladdr((const void*)&avcodec_version, &d) && d.dli_fname) path = d.dli_fname;
#endif
    if (!path.empty()) {
        std::error_code ec;
        const fs::path c = fs::weakly_canonical(u8path(path), ec);
        i.folder = u8str((ec ? u8path(path) : c).parent_path());
    }
    return i;
}

namespace {
bool gRestart = false;
#if defined(__linux__)
// the RUNPATH (or RPATH) of the program, with $ORIGIN filled in: where its own libraries are installed
std::vector<std::string> ownRunpath() {
    std::vector<std::string> v;
    std::string rp;
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) -> int {
        std::string& out = *(std::string*)data;
        const ElfW(Dyn)* dyn = nullptr;
        for (int i = 0; i < info->dlpi_phnum; i++)
            if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) dyn = (const ElfW(Dyn)*)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
        if (!dyn) return 1;
        const char* strtab = nullptr;
        long run = -1, rpath = -1;
        for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; d++) {
            if (d->d_tag == DT_STRTAB) strtab = (const char*)d->d_un.d_ptr;
            if (d->d_tag == DT_RUNPATH) run = (long)d->d_un.d_val;
            if (d->d_tag == DT_RPATH) rpath = (long)d->d_un.d_val;
        }
        if (strtab && (run >= 0 || rpath >= 0)) out = strtab + (run >= 0 ? run : rpath);
        return 1;   // the first object is the program itself
    }, &rp);
    const std::string origin = fs::path(selfExe()).parent_path().string();
    std::stringstream ss(rp);
    for (std::string d; std::getline(ss, d, ':');) {
        for (const std::string_view tok : {std::string_view("$ORIGIN"), std::string_view("${ORIGIN}")})
            for (size_t k = d.find(tok); k != std::string::npos; k = d.find(tok, k + origin.size())) d.replace(k, tok.size(), origin);
        if (!d.empty()) v.push_back(d);
    }
    return v;
}
#endif
}   // namespace

Info own() {
    Info i;
    i.version = FFMPEG_VERSION;   // the headers OnAir is built with are those of its own FFmpeg
    const Lib& codec = kLibs[3];
#ifdef _WIN32
    // the normal DLL search (the program's folder first, then the system folders and PATH), not the module already loaded
    wchar_t p[MAX_PATH * 4];
    wchar_t* file = nullptr;
    const std::wstring name = u8path(fileName(codec)).wstring();
    const DWORD n = SearchPathW(nullptr, name.c_str(), nullptr, (DWORD)(sizeof p / sizeof p[0]), p, &file);
    if (n && n < sizeof p / sizeof p[0]) i.folder = u8str(fs::path(std::wstring(p, n)).parent_path());
#else
    std::vector<std::string> dirs;
    if (const char* lp = getenv(kLoaderVar)) { std::stringstream ss(lp); for (std::string d; std::getline(ss, d, ':');) if (!d.empty()) dirs.push_back(d); }
#if defined(__linux__)
    for (const std::string& d : ownRunpath()) dirs.push_back(d);
#endif
    const fs::path exeDir = fs::path(selfExe()).parent_path();
    dirs.push_back((exeDir / "../Frameworks").string());   // macOS app bundle
    dirs.push_back(exeDir.string());
    dirs.push_back((exeDir / "../lib").string());
    std::error_code ec;
    for (const std::string& d : dirs) {
        if (!gStatus.folder.empty() && sameFolder(d, gStatus.folder)) continue;   // the other FFmpeg in use
        if (fs::exists(fs::path(d) / fileName(codec), ec)) {
            const fs::path c = fs::weakly_canonical(d, ec);
            i.folder = (ec ? fs::path(d) : c).string();
            break;
        }
    }
#endif
    return i;
}

void requestRestart() { gRestart = true; }
bool restartRequested() { return gRestart; }

void restartIfRequested(char** argv) {
    if (!gRestart) return;
    const std::string exe = selfExe();
    if (exe.empty()) return;
    fprintf(stderr, "OnAir: restarting\n");
    fflush(stderr);
#ifdef _WIN32
    (void)argv;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = GetCommandLineW();   // the same command line
    if (CreateProcessW(u8path(exe).wstring().c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else fprintf(stderr, "OnAir: could not restart (%s)\n", winError(GetLastError()).c_str());
#else
    execv(exe.c_str(), argv);
    fprintf(stderr, "OnAir: could not restart (%s)\n", strerror(errno));
#endif
}

}   // namespace ffmpeglibs
