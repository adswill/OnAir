#include "dect2/updater.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
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
    default: return "development build";
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
    int rc = 0;
    const std::string cmd = "curl -fsSL --max-time 20 -H " + shq("Accept: application/vnd.github+json") + " -H " + shq("User-Agent: OnAir/" + current_) + " " + shq(api_);
    const std::string body = runCapture(cmd, &rc);
    if (cancel_) return;
    if (rc != 0 || body.empty()) { setPhase(Phase::Failed, rc == 0 ? "no answer from GitHub" : "could not reach GitHub (is curl installed and the network up?)"); return; }
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
    runCommand("mkdir \"" + dir + "\" 2>nul");
#else
    runCommand("mkdir -p " + shq(dir));
#endif
    file_ = dir + "/" + r.assetName;
    // curl runs in a helper thread while the size of the file is watched
    std::atomic<bool> finished{false};
    std::thread dl([&] { runCommand("curl -fsSL --retry 2 --max-time 1800 -o " + shq(file_) + " " + shq(r.assetUrl)); finished = true; });
    while (!finished) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        std::ifstream f(file_, std::ios::binary | std::ios::ate);
        const uint64_t sz = f ? (uint64_t)f.tellg() : 0;
        std::lock_guard<std::mutex> lk(mu_);
        st_.done = sz;
    }
    dl.join();
    if (cancel_) { setPhase(Phase::Failed, "cancelled"); return; }
    std::string err;
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
        const std::string script = dir + "\\apply.cmd";
        std::ofstream o(script);
        o << "@echo off\r\n"
          << ":wait\r\ntasklist /FI \"PID eq " << pid << "\" 2>nul | find \"" << pid << "\" >nul\r\nif not errorlevel 1 (timeout /t 1 /nobreak >nul & goto wait)\r\n"
          << "powershell -NoProfile -Command \"Start-Process -FilePath '" << staged_ << "' -ArgumentList '/S' -Verb RunAs -Wait\"\r\n";
        if (restart) o << "start \"\" \"" << exe_ << "\"\r\n";
        o.close();
        spawnDetached(script, {});
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
    { std::lock_guard<std::mutex> lk(mu_); url = st_.release.pageUrl; }
    if (url.empty()) url = "https://github.com/adswill/OnAir/releases";
#if defined(_WIN32)
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    runCommand("open " + shq(url) + " >/dev/null 2>&1 &");
#else
    runCommand("xdg-open " + shq(url) + " >/dev/null 2>&1 &");
#endif
}

} // namespace dect2
