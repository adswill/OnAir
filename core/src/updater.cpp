#include "dect2/updater.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <map>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <process.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
#include <functional>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace dect2 {

// ---------------------------------------------------------------- SHA-256

namespace {
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    size_t fill = 0;
    uint64_t total = 0;
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25), ch = (e & f) ^ (~e & g), t1 = hh + S1 + ch + k[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void add(const void* data, size_t n) {
        const uint8_t* p = (const uint8_t*)data;
        total += n;
        while (n) {
            const size_t k = std::min(n, 64 - fill);
            std::memcpy(buf + fill, p, k);
            fill += k; p += k; n -= k;
            if (fill == 64) { block(buf); fill = 0; }
        }
    }
    std::string finish() {
        const uint64_t bits = total * 8;
        const uint8_t one = 0x80, zero = 0;
        add(&one, 1);
        while (fill != 56) add(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
        add(len, 8);
        char out[65];
        for (int i = 0; i < 8; i++) snprintf(out + 8 * i, 9, "%08x", h[i]);
        return out;
    }
};
}

std::string sha256Bytes(const void* data, size_t n) { Sha256 s; s.add(data, n); return s.finish(); }

std::string sha256File(const std::string& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot read " + path; return std::string(); }
    Sha256 s;
    std::vector<char> b(1 << 20);
    while (f) { f.read(b.data(), (std::streamsize)b.size()); const std::streamsize n = f.gcount(); if (n > 0) s.add(b.data(), (size_t)n); }
    return s.finish();
}

// ---------------------------------------------------------------- a small JSON reader

namespace {
struct J {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;
    const J* get(const char* key) const { for (auto& kv : o) if (kv.first == key) return &kv.second; return nullptr; }
    std::string str(const char* key) const { const J* v = get(key); return v && v->t == Str ? v->s : std::string(); }
    double num(const char* key) const { const J* v = get(key); return v && v->t == Num ? v->n : 0; }
    bool flag(const char* key) const { const J* v = get(key); return v && v->t == Bool && v->b; }
};

struct JParser {
    const std::string& s;
    size_t i = 0;
    bool ok = true;
    explicit JParser(const std::string& str) : s(str) {}
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++; }
    bool lit(const char* w) { const size_t n = strlen(w); if (s.compare(i, n, w) == 0) { i += n; return true; } return false; }
    static void utf8(std::string& o, unsigned c) {
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
        else { o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 0x3F)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
    }
    std::string string() {
        std::string o;
        i++;   // the opening quote
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                i++;
                switch (s[i]) {
                case 'n': o += '\n'; break; case 't': o += '\t'; break; case 'r': o += '\r'; break; case 'b': o += '\b'; break; case 'f': o += '\f'; break;
                case 'u': {
                    unsigned c = 0;
                    for (int k = 1; k <= 4 && i + (size_t)k < s.size(); k++) c = c * 16 + (unsigned)(isdigit((unsigned char)s[i + k]) ? s[i + k] - '0' : (tolower(s[i + k]) - 'a' + 10) & 15);
                    i += 4;
                    if (c >= 0xD800 && c < 0xDC00 && i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {   // a surrogate pair
                        unsigned lo = 0;
                        for (int k = 3; k <= 6; k++) lo = lo * 16 + (unsigned)(isdigit((unsigned char)s[i + k]) ? s[i + k] - '0' : (tolower(s[i + k]) - 'a' + 10) & 15);
                        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                    utf8(o, c);
                    break;
                }
                default: o += s[i];
                }
                i++;
            } else o += s[i++];
        }
        if (i < s.size()) i++; else ok = false;
        return o;
    }
    J value(int depth = 0) {
        J v;
        ws();
        if (i >= s.size() || depth > 64) { ok = false; return v; }
        if (s[i] == '{') {
            v.t = J::Obj; i++; ws();
            if (i < s.size() && s[i] == '}') { i++; return v; }
            for (;;) {
                ws();
                if (i >= s.size() || s[i] != '"') { ok = false; return v; }
                std::string k = string();
                ws();
                if (i >= s.size() || s[i] != ':') { ok = false; return v; }
                i++;
                v.o.emplace_back(std::move(k), value(depth + 1));
                if (!ok) return v;
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; return v; }
                ok = false; return v;
            }
        }
        if (s[i] == '[') {
            v.t = J::Arr; i++; ws();
            if (i < s.size() && s[i] == ']') { i++; return v; }
            for (;;) {
                v.a.push_back(value(depth + 1));
                if (!ok) return v;
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; return v; }
                ok = false; return v;
            }
        }
        if (s[i] == '"') { v.t = J::Str; v.s = string(); return v; }
        if (lit("true")) { v.t = J::Bool; v.b = true; return v; }
        if (lit("false")) { v.t = J::Bool; return v; }
        if (lit("null")) return v;
        char* end = nullptr;
        v.n = strtod(s.c_str() + i, &end);
        if (end == s.c_str() + i) { ok = false; return v; }
        v.t = J::Num; i = (size_t)(end - s.c_str());
        return v;
    }
};
}

// ---------------------------------------------------------------- versions and release selection

int compareVersions(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& v) {
        std::vector<long> r;
        size_t i = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
        while (i < v.size()) {
            if (isdigit((unsigned char)v[i])) { long x = 0; while (i < v.size() && isdigit((unsigned char)v[i])) x = x * 10 + (v[i++] - '0'); r.push_back(x); }
            else if (v[i] == '.') i++;
            else break;   // a suffix such as -rc1 is ignored
        }
        return r;
    };
    auto x = parts(a), y = parts(b);
    for (size_t i = 0; i < std::max(x.size(), y.size()); i++) {
        const long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p > q ? 1 : -1;
    }
    return 0;
}

std::string thisArch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#else
    return "x86_64";
#endif
}

namespace {
bool endsWith(const std::string& s, const char* suf) { const size_t n = strlen(suf); return s.size() >= n && s.compare(s.size() - n, n, suf) == 0; }
bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

bool wantedAsset(const std::string& n, InstallKind kind, const std::string& arch) {
    const bool arm = arch == "arm64" || arch == "aarch64";
    switch (kind) {
    case InstallKind::MacApp: return has(n, "macos") && has(n, arm ? "arm64" : "x86_64") && endsWith(n, ".dmg");
    case InstallKind::WindowsInstall: return has(n, "windows") && endsWith(n, "-setup.exe");
    case InstallKind::LinuxPortable: return has(n, "linux") && has(n, arm ? "aarch64" : "x86_64") && endsWith(n, "-portable.tar.gz");
    case InstallKind::LinuxDeb: return endsWith(n, ".deb") && has(n, arm ? "arm64" : "amd64");
    default: return false;
    }
}
}

ReleaseInfo pickRelease(const std::string& json, const std::string& current, bool pre, InstallKind kind, const std::string& arch, std::string* err) {
    ReleaseInfo best;
    JParser p(json);
    J root = p.value();
    if (!p.ok || root.t != J::Arr) {
        if (err) *err = "the answer from GitHub could not be read";
        return best;
    }
    for (const J& r : root.a) {
        if (r.t != J::Obj || r.flag("draft")) continue;
        const bool isPre = r.flag("prerelease");
        if (isPre && !pre) continue;
        const std::string tag = r.str("tag_name");
        if (tag.empty() || compareVersions(tag, current) <= 0) continue;
        if (best.valid && compareVersions(tag, best.tag) <= 0) continue;
        ReleaseInfo c;
        c.tag = tag;
        c.version = (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
        c.name = r.str("name");
        c.notes = r.str("body");
        c.pageUrl = r.str("html_url");
        c.prerelease = isPre;
        if (const J* as = r.get("assets")) {
            for (const J& a : as->a) {
                const std::string n = a.str("name");
                if (!wantedAsset(n, kind, arch)) continue;
                c.assetName = n;
                c.assetUrl = a.str("browser_download_url");
                std::string d = a.str("digest");
                if (d.compare(0, 7, "sha256:") == 0) c.sha256 = d.substr(7);
                c.size = (uint64_t)a.num("size");
                break;
            }
        }
        c.valid = true;
        best = c;
    }
    if (!best.valid && err) err->clear();
    return best;
}

// ---------------------------------------------------------------- where the program is

std::string executablePath() {
#if defined(__APPLE__)
    char b[4096]; uint32_t n = sizeof b;
    if (_NSGetExecutablePath(b, &n) == 0) { char r[4096]; if (realpath(b, r)) return r; return b; }
    return std::string();
#elif defined(_WIN32)
    char b[4096];
    const DWORD n = GetModuleFileNameA(nullptr, b, sizeof b);
    return n ? std::string(b, n) : std::string();
#else
    char b[4096];
    const ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
    return n > 0 ? std::string(b, (size_t)n) : std::string();
#endif
}

namespace {
bool fileExists(const std::string& p) { std::ifstream f(p, std::ios::binary); return (bool)f; }
std::string dirOf(const std::string& p) { const size_t k = p.find_last_of("/\\"); return k == std::string::npos ? std::string(".") : p.substr(0, k); }
}

InstallKind detectInstallKind(const std::string& exe) {
    if (exe.empty()) return InstallKind::Unknown;
#if defined(__APPLE__)
    return exe.find(".app/Contents/MacOS/") != std::string::npos ? InstallKind::MacApp : InstallKind::Unknown;
#elif defined(_WIN32)
    return fileExists(dirOf(exe) + "\\Uninstall.exe") ? InstallKind::WindowsInstall : InstallKind::Unknown;
#else
    if (exe.compare(0, 5, "/usr/") == 0) return InstallKind::LinuxDeb;
    const std::string bin = dirOf(exe), root = dirOf(bin);
    if (bin.size() >= 4 && bin.compare(bin.size() - 4, 4, "/bin") == 0 && fileExists(root + "/lib/../README.txt")) return InstallKind::LinuxPortable;
    return InstallKind::Unknown;
#endif
}

const char* installKindName(InstallKind k) {
    switch (k) {
    case InstallKind::MacApp: return "macOS app";
    case InstallKind::WindowsInstall: return "Windows installation";
    case InstallKind::LinuxPortable: return "portable Linux package";
    case InstallKind::LinuxDeb: return "Debian package";
    default: return "portable or development copy";   // the Windows zip lands here too
    }
}

// ---------------------------------------------------------------- running programs

namespace {
std::string shq(const std::string& s) {   // quoted for the shell (sh) or for cmd
#if defined(_WIN32)
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += "\\\""; else o += c; }
    return o + "\"";
#else
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
#endif
}

FILE* openPipe(const std::string& cmd) {
#if defined(_WIN32)
    return _popen(cmd.c_str(), "rb");
#else
    return popen(cmd.c_str(), "r");
#endif
}
int closePipe(FILE* f) {
#if defined(_WIN32)
    return _pclose(f);
#else
    return pclose(f);
#endif
}

std::string runCapture(const std::string& cmd, int* rc = nullptr) {
    std::string out;
#if defined(_WIN32)
    FILE* f = openPipe("\"" + cmd + "\"");
#else
    FILE* f = openPipe(cmd + " 2>&1");
#endif
    if (!f) { if (rc) *rc = -1; return out; }
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
    const int r = closePipe(f);
    if (rc) *rc = r;
    return out;
}

int runCommand(const std::string& cmd) {
#if defined(_WIN32)
    return system(("\"" + cmd + "\"").c_str());
#else
    return system(cmd.c_str());
#endif
}

void spawnDetached(const std::string& script, const std::vector<std::string>& args) {
#if defined(_WIN32)
    std::string cmd = "cmd.exe /C \"\"" + script + "\"";
    for (auto& a : args) cmd += " \"" + a + "\"";
    cmd += "\"";
    STARTUPINFOA si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<char> c(cmd.begin(), cmd.end());
    c.push_back(0);
    if (CreateProcessA(nullptr, c.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
#else
    const pid_t pid = fork();
    if (pid == 0) {
        setsid();
        const int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        std::vector<char*> av;
        av.push_back((char*)"/bin/sh");
        av.push_back((char*)script.c_str());
        for (auto& a : args) av.push_back((char*)a.c_str());
        av.push_back(nullptr);
        execv("/bin/sh", av.data());
        _exit(127);
    }
#endif
}

// Runs curl with these arguments, its output in *out when asked for. No shell and, on Windows, no console window (OnAir has none, so cmd.exe or
// a console program started through it would open one). tick() is called about every 100 ms; curl is stopped as soon as cancel is set, so that
// closing OnAir never waits for a download. The exit code of curl; kCurlMissing when it could not be started, kCurlCancelled when stopped.
constexpr int kCurlMissing = -2, kCurlCancelled = -1;
int runCurl(const std::vector<std::string>& args, std::string* out, const std::atomic<bool>& cancel, const std::function<void()>& tick = {}) {
#if defined(_WIN32)
    auto wide = [](const std::string& a) {   // the paths come from the system in the ANSI code page
        const int w = MultiByteToWideChar(CP_ACP, 0, a.c_str(), -1, nullptr, 0);
        std::wstring ws((size_t)(w > 0 ? w : 1), L'\0');
        if (w > 0) MultiByteToWideChar(CP_ACP, 0, a.c_str(), -1, &ws[0], w);
        ws.resize(wcslen(ws.c_str()));
        return ws;
    };
    auto quote = [](const std::wstring& a) {   // one argument, as CommandLineToArgvW reads it back
        std::wstring o = L"\"";
        size_t bs = 0;
        for (wchar_t c : a) {
            if (c == L'\\') { bs++; continue; }
            if (c == L'"') { o.append(bs * 2 + 1, L'\\'); o += c; }
            else { o.append(bs, L'\\'); o += c; }
            bs = 0;
        }
        o.append(bs * 2, L'\\');
        return o + L"\"";
    };
    // curl.exe from System32 (Windows 10 1803 and later), else one on the PATH
    wchar_t sys[MAX_PATH + 1] = {0};
    std::wstring exe = GetSystemDirectoryW(sys, MAX_PATH) ? std::wstring(sys) + L"\\curl.exe" : std::wstring();
    if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) exe = L"curl.exe";
    std::wstring cmd = quote(exe);
    for (auto& a : args) cmd += L" " + quote(wide(a));
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (out && !CreatePipe(&rd, &wr, &sa, 1 << 20)) return kCurlMissing;
    if (rd) SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr ? wr : nul;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    const BOOL started = CreateProcessW(exe.find(L'\\') != std::wstring::npos ? exe.c_str() : nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (wr) CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) { if (rd) CloseHandle(rd); return kCurlMissing; }
    char b[16384];
    for (;;) {
        if (rd) { DWORD avail = 0, n = 0; while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail && ReadFile(rd, b, (DWORD)std::min<size_t>(avail, sizeof b), &n, nullptr) && n) out->append(b, n); }
        if (cancel) TerminateProcess(pi.hProcess, 1);
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) break;
        if (tick) tick();
    }
    if (rd) { DWORD n = 0; while (ReadFile(rd, b, sizeof b, &n, nullptr) && n) out->append(b, n); CloseHandle(rd); }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return cancel ? kCurlCancelled : (int)code;
#else
    int fd[2] = {-1, -1};
    if (out && pipe(fd) != 0) return kCurlMissing;
    std::vector<char*> av;
    av.push_back((char*)"curl");
    for (auto& a : args) av.push_back((char*)a.c_str());
    av.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (out) { posix_spawn_file_actions_adddup2(&fa, fd[1], 1); posix_spawn_file_actions_addclose(&fa, fd[0]); posix_spawn_file_actions_addclose(&fa, fd[1]); }
    else posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = -1;
    const int e = posix_spawnp(&pid, "curl", &fa, nullptr, av.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (out) close(fd[1]);
    if (e != 0) { if (out) close(fd[0]); return kCurlMissing; }
    int status = 0;
    bool eof = !out;
    char b[16384];
    for (;;) {
        if (!eof) {
            pollfd p{fd[0], POLLIN, 0};
            if (poll(&p, 1, 100) > 0) {
                const ssize_t n = read(fd[0], b, sizeof b);
                if (n > 0) out->append(b, (size_t)n);
                else if (n == 0 || errno != EINTR) eof = true;
            }
        } else std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (cancel) kill(pid, SIGTERM);
        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid || (w < 0 && errno != EINTR)) break;
        if (tick) tick();
    }
    if (out) { ssize_t n; while ((n = read(fd[0], b, sizeof b)) > 0) out->append(b, (size_t)n); close(fd[0]); }
    if (cancel) return kCurlCancelled;
    if (!WIFEXITED(status) || WEXITSTATUS(status) == 127) return kCurlMissing;   // 127: the child could not run curl
    return WEXITSTATUS(status);
#endif
}

// Why curl failed, in words.
std::string curlError(int rc, const char* what) {
    if (rc == kCurlMissing)
#if defined(_WIN32)
        return std::string("curl.exe was not found (Windows 10 version 1803 and later have it): ") + what + " needs it";
#else
        return std::string("the curl program was not found: ") + what + " needs it (install curl)";
#endif
    if (rc == 6 || rc == 7 || rc == 28 || rc == 35 || rc == 56) return std::string("could not reach GitHub (") + (rc == 28 ? "it timed out;" : "is the network up?") + " curl error " + std::to_string(rc) + ")";
    if (rc == 22) return std::string(what) + ": GitHub answered with an error (it limits how often it may be asked: try again later)";
    return std::string(what) + " failed (curl error " + std::to_string(rc) + ")";
}

std::string tempRoot() {
#if defined(_WIN32)
    const char* t = getenv("TEMP");
    return t ? t : "C:\\Windows\\Temp";
#else
    const char* t = getenv("TMPDIR");
    return t && *t ? std::string(t) : "/tmp";
#endif
}

int pidNow() {
#if defined(_WIN32)
    return (int)GetCurrentProcessId();
#else
    return (int)getpid();
#endif
}

double nowSeconds() { return (double)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
}


// ---------------------------------------------------------------- the Windows helper script and its result

namespace {
// A value for `set "NAME=value"` in a batch file: no quotes or line breaks, % doubled, slashes as Windows writes them.
std::string batchValue(const std::string& v, bool path) {
    std::string o;
    for (char c : v) {
        if (c == '"' || c == '\r' || c == '\n') continue;
        if (c == '%') o += "%%";
        else if (path && c == '/') o += '\\';
        else o += c;
    }
    return o;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
}

std::string windowsApplyScript(int pid, const std::string& setupExe, const std::string& installedExe, const std::string& resultFile, const std::string& version, bool restart) {
    std::ostringstream o;
    o << "@echo off\r\n"
      << "chcp 65001 >nul\r\n"          // the values below are UTF-8
      << "setlocal\r\n"
      << "set \"PID=" << pid << "\"\r\n"
      << "set \"SETUP=" << batchValue(setupExe, true) << "\"\r\n"
      << "set \"EXE=" << batchValue(installedExe, true) << "\"\r\n"
      << "set \"RESULT=" << batchValue(resultFile, true) << "\"\r\n"
      << "set \"VER=" << batchValue(version, false) << "\"\r\n"
      << "set \"RESTART=" << (restart ? 1 : 0) << "\"\r\n"
      // up to two minutes for OnAir to end: the setup program cannot replace files that are in use
      << "set /a N=0\r\n"
      << ":wait\r\n"
      << "tasklist /FI \"PID eq %PID%\" 2>nul | find \"%PID%\" >nul\r\n"
      << "if errorlevel 1 goto run\r\n"
      << "set /a N+=1\r\n"
      << "if %N% GEQ 120 goto run\r\n"
      << "ping -n 2 127.0.0.1 >nul\r\n"
      << "goto wait\r\n"
      << ":run\r\n"
      << "set \"CODE=-2\"\r\n"
      << "if not exist \"%SETUP%\" goto record\r\n"
      << "start \"\" /wait \"%SETUP%\" /S\r\n"   // start /wait: the setup program is a window program, cmd would not wait for it otherwise
      << "set \"CODE=%ERRORLEVEL%\"\r\n"
      << ":record\r\n"
      << ">\"%RESULT%\" echo version=%VER%\r\n"
      << ">>\"%RESULT%\" echo setup_exit=%CODE%\r\n"
      << ">>\"%RESULT%\" echo time=%DATE% %TIME%\r\n"
      // through explorer.exe the program starts with the rights of the user, not with the administrator's
      << "if not \"%RESTART%\"==\"1\" goto done\r\n"
      << "start \"\" explorer.exe \"%EXE%\"\r\n"
      << ":done\r\n"
      << "endlocal\r\n";
    return o.str();
}

std::string windowsApplyParams(const std::string& scriptPath) {
    std::string p;
    for (char c : scriptPath) if (c != '"') p += c == '/' ? '\\' : c;
    return "/C \"\"" + p + "\"\"";   // cmd removes the outer pair of quotes
}

UpdateResult parseUpdateResult(const std::string& text) {
    UpdateResult r;
    bool haveExit = false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && (unsigned char)line[0] == 0xEF && line.size() >= 3) line.erase(0, 3);   // a byte order mark
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "version") r.version = v;
        else if (k == "setup_exit") { char* end = nullptr; const long x = strtol(v.c_str(), &end, 10); if (end != v.c_str()) { r.setupExit = (int)x; haveExit = true; } }
        else if (k == "time") r.time = v;
    }
    r.found = !r.version.empty() && haveExit;
    r.ok = r.found && r.setupExit == 0;
    return r;
}

std::string updateResultPath() { const std::string t = tempRoot(); return t + (t.back() == '/' || t.back() == '\\' ? "" : "/") + "onair-update-result.txt"; }

// ---------------------------------------------------------------- the updater

Updater::Updater(const std::string& v) : current_(v) {
    api_ = "https://api.github.com/repos/adswill/OnAir/releases?per_page=12";
    if (const char* e = getenv("ONAIR_UPDATE_URL")) if (*e) api_ = e;
    setExecutable(executablePath());
}

Updater::~Updater() { cancel_ = true; if (th_.joinable()) th_.join(); }

void Updater::setExecutable(const std::string& p, int kindOverride) {
    exe_ = p;
    kind_ = kindOverride >= 0 ? (InstallKind)kindOverride : detectInstallKind(p);
    std::lock_guard<std::mutex> lk(mu_);
    st_.kind = kind_;
    st_.autoInstallable = kind_ == InstallKind::MacApp || kind_ == InstallKind::WindowsInstall || kind_ == InstallKind::LinuxPortable;
}

Updater::Status Updater::status() const { std::lock_guard<std::mutex> lk(mu_); return st_; }

void Updater::setPhase(Phase p, const std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    st_.phase = p;
    st_.error = err;
}

void Updater::cancel() { cancel_ = true; }

void Updater::loadResult() {
    const std::string path = updateResultPath();
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    std::remove(path.c_str());   // shown once
    const UpdateResult r = parseUpdateResult(text);
    std::lock_guard<std::mutex> lk(mu_);
    st_.lastResult = r;
}

void Updater::dismissResult() { std::lock_guard<std::mutex> lk(mu_); st_.lastResult = UpdateResult(); }

void Updater::check(bool pre) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (st_.phase == Phase::Checking || st_.phase == Phase::Downloading || st_.phase == Phase::Preparing) return;
    }
    if (th_.joinable()) th_.join();
    cancel_ = false;
    setPhase(Phase::Checking);
    th_ = std::thread([this, pre] { runCheck(pre); });
}

void Updater::runCheck(bool pre) {
    std::string body;
    const int rc = runCurl({"-fsSL", "--max-time", "20", "-H", "Accept: application/vnd.github+json", "-H", "User-Agent: OnAir/" + current_, api_}, &body, cancel_);
    if (cancel_) return;
    if (rc != 0 || body.empty()) { setPhase(Phase::Failed, rc == 0 ? "no answer from GitHub" : curlError(rc, "the update check")); return; }
    std::string err;
    ReleaseInfo r = pickRelease(body, current_, pre, kind_, thisArch(), &err);
    std::lock_guard<std::mutex> lk(mu_);
    st_.checkedAt = nowSeconds();
    if (!err.empty()) { st_.phase = Phase::Failed; st_.error = err; return; }
    st_.release = r;
    st_.phase = r.valid ? Phase::Available : Phase::UpToDate;
    st_.error.clear();
}

std::string Updater::workDir() const { return tempRoot() + (tempRoot().back() == '/' || tempRoot().back() == '\\' ? "" : "/") + "onair-update-" + std::to_string(pidNow()); }

void Updater::download() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (st_.phase != Phase::Available || !st_.release.valid) return;
    }
    if (th_.joinable()) th_.join();
    cancel_ = false;
    setPhase(Phase::Downloading);
    th_ = std::thread([this] { runDownload(); });
}

void Updater::runDownload() {
    ReleaseInfo r;
    { std::lock_guard<std::mutex> lk(mu_); r = st_.release; st_.done = 0; st_.total = r.size; }
    if (r.assetUrl.empty()) { setPhase(Phase::Failed, "this release has no package for this computer"); return; }
    if (r.sha256.size() != 64 && api_.compare(0, 7, "file://") != 0) { setPhase(Phase::Failed, "GitHub lists no checksum for the package: not installing it"); return; }
    const std::string dir = workDir();
#if defined(_WIN32)
    CreateDirectoryA(dir.c_str(), nullptr);   // not through cmd: that would open a console window
#else
    runCommand("mkdir -p " + shq(dir));
#endif
    file_ = dir + "/" + r.assetName;
    // the size of the file is watched while curl runs
    const int rc = runCurl({"-fsSL", "--retry", "2", "--max-time", "1800", "-o", file_, r.assetUrl}, nullptr, cancel_, [&] {
        std::ifstream f(file_, std::ios::binary | std::ios::ate);
        const uint64_t sz = f ? (uint64_t)f.tellg() : 0;
        std::lock_guard<std::mutex> lk(mu_);
        st_.done = sz;
    });
    if (cancel_) { setPhase(Phase::Failed, "cancelled"); std::remove(file_.c_str()); return; }
    if (rc != 0) { setPhase(Phase::Failed, curlError(rc, "the download")); std::remove(file_.c_str()); return; }
    std::string err;
    {
        std::ifstream f(file_, std::ios::binary | std::ios::ate);
        const uint64_t sz = f ? (uint64_t)f.tellg() : 0;
        if (r.size && sz != r.size) { setPhase(Phase::Failed, "the downloaded file has the wrong size (" + std::to_string(sz) + " of " + std::to_string(r.size) + " bytes): not installing it"); std::remove(file_.c_str()); return; }
    }
    const std::string got = sha256File(file_, &err);
    if (got.empty()) { setPhase(Phase::Failed, err.empty() ? "the download is missing" : err); return; }
    if (r.sha256.size() == 64 && got != r.sha256) { setPhase(Phase::Failed, "the downloaded file does not match its checksum: not installing it"); std::remove(file_.c_str()); return; }
    { std::lock_guard<std::mutex> lk(mu_); st_.done = st_.total = std::max<uint64_t>(st_.total, st_.done); }
    setPhase(Phase::Preparing);
    if (!prepare(file_, err)) { setPhase(Phase::Failed, err); return; }
    setPhase(Phase::Ready);
}

bool Updater::prepare(const std::string& file, std::string& err) {
    const std::string dir = workDir();
    switch (kind_) {
    case InstallKind::MacApp: {
        // the new app is copied from the disk image next to the installed one, on the same volume, so that swapping them is a rename
        const std::string appPath = exe_.substr(0, exe_.find(".app/Contents/MacOS/") + 4);
        const std::string parent = dirOf(appPath);
        const std::string stage = parent + "/.OnAir-update";
        const std::string mnt = dir + "/mnt";
        runCommand("rm -rf " + shq(stage) + " && mkdir -p " + shq(stage) + " " + shq(mnt));
        int rc = 0;
        const std::string out = runCapture("hdiutil attach -nobrowse -readonly -noverify -mountpoint " + shq(mnt) + " " + shq(file), &rc);
        if (rc != 0) { err = "the disk image could not be opened: " + out.substr(0, 120); return false; }
        const std::string name = appPath.substr(appPath.find_last_of('/') + 1);
        std::string src = mnt + "/" + name;
        if (!fileExists(src + "/Contents/Info.plist")) src = mnt + "/OnAir.app";
        const int cp = runCommand("ditto " + shq(src) + " " + shq(stage + "/" + name) + " >/dev/null 2>&1");
        runCommand("hdiutil detach " + shq(mnt) + " -quiet >/dev/null 2>&1");
        if (cp != 0 || !fileExists(stage + "/" + name + "/Contents/Info.plist")) { err = "the new version could not be copied next to the installed one (no permission? it has to be written to " + parent + ")"; runCommand("rm -rf " + shq(stage)); return false; }
        runCommand("xattr -cr " + shq(stage + "/" + name) + " >/dev/null 2>&1");
        staged_ = stage + "/" + name;
        return true;
    }
    case InstallKind::LinuxPortable: {
        const std::string root = dirOf(dirOf(exe_));
        const std::string parent = dirOf(root);
        const std::string stage = parent + "/.onair-update";
        runCommand("rm -rf " + shq(stage) + " && mkdir -p " + shq(stage));
        if (runCommand("tar -xzf " + shq(file) + " -C " + shq(stage)) != 0) { err = "the package could not be unpacked (no permission to write in " + parent + "?)"; return false; }
        // the archive holds one folder
        std::string top = runCapture("ls " + shq(stage));
        top.erase(std::remove(top.begin(), top.end(), '\n'), top.end());
        if (top.empty() || !fileExists(stage + "/" + top + "/bin/onair")) { err = "the package has an unexpected layout"; return false; }
        staged_ = stage + "/" + top;
        return true;
    }
    case InstallKind::WindowsInstall:
    case InstallKind::LinuxDeb:
        staged_ = file;
        return true;
    default:
        err = "this copy of OnAir cannot replace itself";
        return false;
    }
}

bool Updater::apply(bool restart, std::string* err) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (st_.phase != Phase::Ready) { if (err) *err = "no update is ready"; return false; }
    }
    const std::string dir = workDir();
    const std::string pid = std::to_string(waitPid_ ? waitPid_ : pidNow());
#if defined(_WIN32)
    if (kind_ == InstallKind::WindowsInstall) {
        auto fail = [&](const std::string& why) { { std::lock_guard<std::mutex> lk(mu_); st_.applyError = why; } if (err) *err = why; return false; };
        std::string version, sum;
        { std::lock_guard<std::mutex> lk(mu_); version = st_.release.version; sum = st_.release.sha256; st_.applyError.clear(); }
        // the file sits in a folder the user can write to and is about to run with administrator rights: look at it once more
        if (sum.size() == 64 && sha256File(staged_) != sum) return fail("Update not installed: the downloaded file changed after it was checked");
        const std::string result = updateResultPath();
        std::remove(result.c_str());
        const std::string script = dir + "\\apply.cmd";
        {
            std::ofstream o(script, std::ios::binary);
            // the paths come from the system in the ANSI code page; the script is UTF-8
            auto utf8 = [](const std::string& a) {
                if (a.empty()) return a;
                const int w = MultiByteToWideChar(CP_ACP, 0, a.c_str(), (int)a.size(), nullptr, 0);
                if (w <= 0) return a;
                std::wstring ws((size_t)w, L'\0');
                MultiByteToWideChar(CP_ACP, 0, a.c_str(), (int)a.size(), &ws[0], w);
                const int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), w, nullptr, 0, nullptr, nullptr);
                if (n <= 0) return a;
                std::string u((size_t)n, '\0');
                WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), w, &u[0], n, nullptr, nullptr);
                return u;
            };
            o << windowsApplyScript((int)std::stoi(pid), utf8(staged_), utf8(exe_), utf8(result), version, restart);
            if (!o) return fail("Update not installed: the installer script could not be written to " + dir);
        }
        // Elevated from here, while the window of OnAir is still in front: the UAC question appears at once and in the foreground.
        // The elevated script waits for this program to end, so it can be asked now.
        auto wide = [](const std::string& a) {
            const int w = MultiByteToWideChar(CP_ACP, 0, a.c_str(), -1, nullptr, 0);
            std::wstring ws((size_t)(w > 0 ? w : 1), L'\0');
            if (w > 0) MultiByteToWideChar(CP_ACP, 0, a.c_str(), -1, &ws[0], w);
            ws.resize(wcslen(ws.c_str()));
            return ws;
        };
        const std::wstring params = wide(windowsApplyParams(script)), folder = wide(dir);
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof sei;
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        sei.hwnd = GetActiveWindow();
        sei.lpVerb = L"runas";
        sei.lpFile = L"cmd.exe";
        sei.lpParameters = params.c_str();
        sei.lpDirectory = folder.c_str();
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei)) {
            const DWORD e = GetLastError();
            if (e == ERROR_CANCELLED) return fail("Update not installed: Windows did not allow the installer to run (administrator rights refused)");
            return fail("Update not installed: Windows did not allow the installer to run (error " + std::to_string((unsigned long)e) + ")");
        }
        if (sei.hProcess) CloseHandle(sei.hProcess);
        return true;
    }
#else
    if (kind_ == InstallKind::MacApp || kind_ == InstallKind::LinuxPortable) {
        const bool mac = kind_ == InstallKind::MacApp;
        const std::string target = mac ? exe_.substr(0, exe_.find(".app/Contents/MacOS/") + 4) : dirOf(dirOf(exe_));
        const std::string script = dir + "/apply.sh";
        std::ofstream o(script);
        o << "#!/bin/sh\n"
          << "PID=$1; TARGET=\"$2\"; NEW=\"$3\"; RESTART=$4; LAUNCH=\"$5\"\n"
          << "i=0; while kill -0 \"$PID\" 2>/dev/null && [ $i -lt 240 ]; do sleep 0.5; i=$((i+1)); done\n"
          << "OLD=\"$TARGET.old-$$\"\n"
          << "if mv \"$TARGET\" \"$OLD\"; then\n"
          << "  if mv \"$NEW\" \"$TARGET\"; then rm -rf \"$OLD\"; else mv \"$OLD\" \"$TARGET\"; fi\n"
          << "fi\n"
          << "rmdir \"$(dirname \"$NEW\")\" 2>/dev/null\n";
        if (mac) o << "if [ \"$RESTART\" = 1 ]; then open \"$TARGET\"; fi\n";
        else o << "if [ \"$RESTART\" = 1 ]; then \"$LAUNCH\" >/dev/null 2>&1 & fi\n";
        o << "rm -rf \"" << dir << "\"\n";
        o.close();
        chmod(script.c_str(), 0755);
        const std::string launch = mac ? std::string() : target + "/bin/onair";
        spawnDetached(script, {pid, target, staged_, restart ? "1" : "0", launch});
        return true;
    }
    if (kind_ == InstallKind::LinuxDeb) {
        // the package manager needs the administrator: polkit asks for the password in a window
        const std::string script = dir + "/apply.sh";
        std::ofstream o(script);
        o << "#!/bin/sh\npkexec dpkg -i " << shq(staged_) << " && { if [ \"$1\" = 1 ]; then onair >/dev/null 2>&1 & fi; }\n";
        o.close();
        chmod(script.c_str(), 0755);
        spawnDetached(script, {restart ? "1" : "0"});
        return true;
    }
#endif
    if (err) *err = "this copy of OnAir cannot replace itself";
    return false;
}

void Updater::openReleasePage() const {
    std::string url;
    {
        std::lock_guard<std::mutex> lk(mu_);
        url = st_.release.pageUrl;
        // after a failed installation the page of the version that did not install
        if (url.empty() && st_.lastResult.found && !st_.lastResult.ok) url = "https://github.com/adswill/OnAir/releases/tag/v" + st_.lastResult.version;
    }
    if (url.empty()) url = "https://github.com/adswill/OnAir/releases";
    openUrl(url);
}

void openUrl(const std::string& url) {
#if defined(_WIN32)
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    runCommand("open " + shq(url) + " >/dev/null 2>&1 &");
#else
    runCommand("xdg-open " + shq(url) + " >/dev/null 2>&1 &");
#endif
}

} // namespace dect2
