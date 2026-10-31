// Linux and Windows services. Linux: dialogs through zenity/kdialog, settings in ~/.config/onair/settings.conf.
// Windows: the standard Open/Save dialogs, settings in %APPDATA%\\OnAir\\settings.conf.
#include "platform.h"
#include "prefs_text.h"
#include "dect2/common/png.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <sys/stat.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <direct.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace plat {

#ifdef _WIN32
namespace { std::string utf8(const std::wstring& w); std::wstring wide(const std::string& s); }
#endif

bool fetchUrl(const std::string& url, const std::string& file, const std::string& userAgent) {
#ifdef _WIN32
    // curl.exe from System32 (not one that happens to be first on the PATH), started directly: a console window would flash up for every tile
    wchar_t sys[MAX_PATH + 1] = {0};
    std::wstring exe = GetSystemDirectoryW(sys, MAX_PATH) ? std::wstring(sys) + L"\\curl.exe" : std::wstring(L"curl.exe");
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) exe = L"curl.exe";   // Windows before 10 1803: a curl the user installed
    std::wstring cmd = L"\"" + exe + L"\" -fsSL --ssl-no-revoke --max-time 12 -A \"" + wide(userAgent) + L"\" -o \"" + wide(file) + L"\" \"" + wide(url) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    if (WaitForSingleObject(pi.hProcess, 15000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return rc == 0;
#else
    auto q = [](const std::string& s) { std::string o = "'"; for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; } return o + "'"; };
    const std::string cmd = "curl -fsSL --max-time 12 -A " + q(userAgent) + " -o " + q(file) + " " + q(url) + " 2>/dev/null";
    return system(cmd.c_str()) == 0;
#endif
}

// reply: when not null, the response body goes there (up to 64 KB)
static bool postImpl(const std::string& url, const std::string& json, const std::string& userAgent, std::string* reply) {
    static std::atomic<unsigned> counter{0};   // the map upload and the automations' webhooks may post at the same time
    const std::string file = cacheDir() + "/post_" + std::to_string(counter++ % 1000) + ".json";
    const std::string outFile = file + ".out";
    auto readReply = [&]() {
        if (!reply) return;
        reply->clear();
#ifdef _WIN32
        FILE* f = _wfopen(wide(outFile).c_str(), L"rb");
#else
        FILE* f = fopen(outFile.c_str(), "rb");
#endif
        if (f) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0 && reply->size() < 65536) reply->append(b, n); fclose(f); }
    };
#ifdef _WIN32
    {
        FILE* f = _wfopen(wide(file).c_str(), L"wb");
        if (!f) return false;
        fwrite(json.data(), 1, json.size(), f);
        fclose(f);
    }
    wchar_t sys[MAX_PATH + 1] = {0};
    std::wstring exe = GetSystemDirectoryW(sys, MAX_PATH) ? std::wstring(sys) + L"\\curl.exe" : std::wstring(L"curl.exe");
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) exe = L"curl.exe";
    std::wstring cmd = L"\"" + exe + L"\" -fsSL --max-time 12 -A \"" + wide(userAgent) +
                       L"\" -H \"Content-Type: application/json\" --data-binary @\"" + wide(file) + L"\" -o \"" + (reply ? wide(outFile) : std::wstring(L"NUL")) + L"\" \"" + wide(url) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring line = cmd;
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    if (WaitForSingleObject(pi.hProcess, 15000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DeleteFileW(wide(file).c_str());
    readReply();
    if (reply) DeleteFileW(wide(outFile).c_str());
    return rc == 0;
#else
    {
        FILE* f = fopen(file.c_str(), "wb");
        if (!f) return false;
        fwrite(json.data(), 1, json.size(), f);
        fclose(f);
    }
    auto q = [](const std::string& s) { std::string o = "'"; for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; } return o + "'"; };
    const std::string cmd = "curl -fsSL --max-time 12 -A " + q(userAgent) +
                            " -H 'Content-Type: application/json' --data-binary @" + q(file) + " -o " + (reply ? q(outFile) : std::string("/dev/null")) + " " + q(url) + " 2>/dev/null";
    const int rc = system(cmd.c_str());
    std::remove(file.c_str());
    readReply();
    if (reply) std::remove(outFile.c_str());
    return rc == 0;
#endif
}

bool postRequest(const std::string& url, const std::string& body, const std::string& contentType, const std::vector<std::string>& headers, const std::string& userAgent) {
    static std::atomic<unsigned> counter{0};
    const unsigned n = counter++ % 1000;
    const std::string bodyFile = cacheDir() + "/postr_" + std::to_string(n) + ".dat", hdrFile = cacheDir() + "/postr_" + std::to_string(n) + ".hdr";
    auto put = [](const std::string& path, const std::string& data, bool priv) {
#ifdef _WIN32
        FILE* f = _wfopen(wide(path).c_str(), L"wb");
        (void)priv;
#else
        FILE* f = fopen(path.c_str(), "wb");
        if (f && priv) fchmod(fileno(f), 0600);
#endif
        if (!f) return false;
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
        return true;
    };
    std::string hdr = "Content-Type: " + (contentType.empty() ? std::string("application/json") : contentType) + "\n";
    for (const std::string& h : headers) {
        if (h.find('\n') != std::string::npos || h.find('\r') != std::string::npos) return false;
        hdr += h + "\n";
    }
    if (!put(bodyFile, body, false) || !put(hdrFile, hdr, true)) return false;
#ifdef _WIN32
    wchar_t sys[MAX_PATH + 1] = {0};
    std::wstring exe = GetSystemDirectoryW(sys, MAX_PATH) ? std::wstring(sys) + L"\\curl.exe" : std::wstring(L"curl.exe");
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) exe = L"curl.exe";
    std::wstring line = L"\"" + exe + L"\" -fsSL --max-time 12 -A \"" + wide(userAgent) + L"\" -H @\"" + wide(hdrFile) + L"\" --data-binary @\"" + wide(bodyFile) + L"\" -o NUL \"" + wide(url) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    bool ok = false;
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (WaitForSingleObject(pi.hProcess, 15000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
        DWORD rc = 1;
        GetExitCodeProcess(pi.hProcess, &rc);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ok = rc == 0;
    }
    DeleteFileW(wide(bodyFile).c_str());
    DeleteFileW(wide(hdrFile).c_str());
    return ok;
#else
    auto q = [](const std::string& s) { std::string o = "'"; for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; } return o + "'"; };
    const std::string cmd = "curl -fsSL --max-time 12 -A " + q(userAgent) + " -H @" + q(hdrFile) + " --data-binary @" + q(bodyFile) + " -o /dev/null " + q(url) + " 2>/dev/null";
    const int rc = system(cmd.c_str());
    std::remove(bodyFile.c_str());
    std::remove(hdrFile.c_str());
    return rc == 0;
#endif
}

// curl exit codes: 6 host not found, 7 cannot connect, 28 timed out, 35/51/58/60 TLS
HttpResult httpRequest(const std::string& method, const std::string& url, const std::string& body, const std::string& userAgent, int timeoutSec) {
    static std::atomic<unsigned> counter{0};
    HttpResult r;
    const std::string base = cacheDir() + "/http_" + std::to_string(counter++ % 1000);
    const std::string inFile = base + ".in", outFile = base + ".out", hdrFile = base + ".hdr";
    const bool post = method == "POST", head = method == "HEAD";
    const std::string to = std::to_string(std::max(1, timeoutSec));
#ifdef _WIN32
    if (post) {
        FILE* f = _wfopen(wide(inFile).c_str(), L"wb");
        if (!f) { r.net = 4; r.detail = "cannot write a temporary file"; return r; }
        fwrite(body.data(), 1, body.size(), f);
        fclose(f);
    }
    wchar_t sys[MAX_PATH + 1] = {0};
    std::wstring exe = GetSystemDirectoryW(sys, MAX_PATH) ? std::wstring(sys) + L"\\curl.exe" : std::wstring(L"curl.exe");
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) exe = L"curl.exe";
    std::wstring line = L"\"" + exe + L"\" -sS --max-time " + wide(to) + L" -A \"" + wide(userAgent) + L"\" " + (head ? L"-I " : L"") +
                        (post ? L"-H \"Content-Type: application/json\" --data-binary @\"" + wide(inFile) + L"\" " : std::wstring()) +
                        L"-D \"" + wide(hdrFile) + L"\" -o \"" + (head ? std::wstring(L"NUL") : wide(outFile)) + L"\" \"" + wide(url) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    DWORD rcw = 1;
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (WaitForSingleObject(pi.hProcess, (timeoutSec + 3) * 1000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 28);
        GetExitCodeProcess(pi.hProcess, &rcw);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    const int rc = (int)rcw;
    auto slurp = [&](const std::string& path, size_t cap) {
        std::string o;
        FILE* f = _wfopen(wide(path).c_str(), L"rb");
        if (f) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0 && o.size() < cap) o.append(b, n); fclose(f); }
        DeleteFileW(wide(path).c_str());
        return o;
    };
    if (post) DeleteFileW(wide(inFile).c_str());
#else
    if (post) {
        FILE* f = fopen(inFile.c_str(), "wb");
        if (!f) { r.net = 4; r.detail = "cannot write a temporary file"; return r; }
        fwrite(body.data(), 1, body.size(), f);
        fclose(f);
    }
    auto q = [](const std::string& s) { std::string o = "'"; for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; } return o + "'"; };
    const std::string cmd = "curl -sS --max-time " + to + " -A " + q(userAgent) + (head ? " -I" : "") +
                            (post ? " -H 'Content-Type: application/json' --data-binary @" + q(inFile) : std::string()) +
                            " -D " + q(hdrFile) + " -o " + (head ? std::string("/dev/null") : q(outFile)) + " " + q(url) + " 2>/dev/null";
    const int st = system(cmd.c_str());
    const int rc = st == -1 ? 1 : (WIFEXITED(st) ? WEXITSTATUS(st) : 1);
    auto slurp = [&](const std::string& path, size_t cap) {
        std::string o;
        FILE* f = fopen(path.c_str(), "rb");
        if (f) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0 && o.size() < cap) o.append(b, n); fclose(f); }
        std::remove(path.c_str());
        return o;
    };
    if (post) std::remove(inFile.c_str());
#endif
    const std::string hdr = slurp(hdrFile, 16384);
    if (!head) r.body = slurp(outFile, 65536);
    // the last status line (a redirect or a 100-continue puts earlier ones before it)
    for (size_t p = 0; p < hdr.size();) {
        size_t e = hdr.find('\n', p);
        if (e == std::string::npos) e = hdr.size();
        if (hdr.compare(p, 5, "HTTP/") == 0) {
            const size_t sp = hdr.find(' ', p);
            if (sp != std::string::npos && sp < e) r.status = std::atoi(hdr.c_str() + sp + 1);
        }
        p = e + 1;
    }
    if (rc == 0 && r.status > 0) return r;
    r.status = 0;
    if (rc == 6 || rc == 5) { r.net = 1; r.detail = "host name not found"; }
    else if (rc == 7) { r.net = 2; r.detail = "connection refused or unreachable"; }
    else if (rc == 28) { r.net = 3; r.detail = "timed out"; }
    else if (rc != 0) { r.net = 4; r.detail = "curl error " + std::to_string(rc); }
    else { r.net = 4; r.detail = "no HTTP status in the answer"; }
    return r;
}
bool postJson(const std::string& url, const std::string& json, const std::string& userAgent) { return postImpl(url, json, userAgent, nullptr); }
bool postJsonReply(const std::string& url, const std::string& json, const std::string& userAgent, std::string& reply) { return postImpl(url, json, userAgent, &reply); }

// the map tiles are PNG: OnAir's own decoder (these systems have none it could call without an extra library)
bool decodeImage(const std::string& path, int& w, int& h, std::vector<uint32_t>& rgba) {
    std::ifstream f(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
    if (!f) return false;
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<uint8_t> px;
    if (!dect2::decodePng(data.data(), data.size(), w, h, px) || w > 4096 || h > 4096) return false;
    rgba.resize((size_t)w * h);
    memcpy(rgba.data(), px.data(), px.size());   // bytes R, G, B, A in memory, as on macOS
    return true;
}

// Windows: %LOCALAPPDATA%\OnAir\cache; Linux: $XDG_CACHE_HOME/onair or ~/.cache/onair. Paths are UTF-8.
std::string cacheDir() {
    std::string d;
#ifdef _WIN32
    if (const wchar_t* la = _wgetenv(L"LOCALAPPDATA")) d = utf8(la) + "\\OnAir\\cache";
    else { wchar_t t[MAX_PATH + 1]; const DWORD n = GetTempPathW(MAX_PATH, t); d = utf8(std::wstring(t, n)) + "OnAir"; }
#else
    const char* x = getenv("XDG_CACHE_HOME");
    const char* h = getenv("HOME");
    d = x && *x ? std::string(x) + "/onair" : std::string(h ? h : "/tmp") + "/.cache/onair";
#endif
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(d.c_str())), ec);
    return d;
}
// Windows: %LOCALAPPDATA%\OnAir; Linux: $XDG_DATA_HOME/onair or ~/.local/share/onair. Paths are UTF-8.
std::string dataDir() {
    std::string d;
    // DECT2_DATA_DIR: another folder for everything OnAir writes (test runs keep away from the user's own data)
    if (const char* o = getenv("DECT2_DATA_DIR"); o && *o) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(o)), ec);
        return o;
    }
#ifdef _WIN32
    if (const wchar_t* la = _wgetenv(L"LOCALAPPDATA")) d = utf8(la) + "\\OnAir";
    else return cacheDir();
#else
    const char* x = getenv("XDG_DATA_HOME");
    const char* h = getenv("HOME");
    d = x && *x ? std::string(x) + "/onair" : std::string(h ? h : "/tmp") + "/.local/share/onair";
#endif
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(d.c_str())), ec);
    return d;
}
void locateStart() {}
int locateState(double&, double&, double&, std::string& msg) { msg = "this system has no location service in OnAir yet: enter the position by hand"; return 3; }

namespace {

#ifdef _WIN32
std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string r((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), r.data(), n, nullptr, nullptr);
    return r;
}
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring r((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), r.data(), n);
    return r;
}

std::string configDir() {   // UTF-8, like every path here: getenv() would give the ANSI code page and lose a user name such as "Jūratė"
    const wchar_t* a = _wgetenv(L"APPDATA");
    return (a && *a ? utf8(a) : std::string(".")) + "/OnAir";
}

void makeDirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/' || path[i] == '\\') _wmkdir(wide(path.substr(0, i)).c_str());
}

bool replaceFile(const std::string& from, const std::string& to) { return MoveFileExW(wide(from).c_str(), wide(to).c_str(), MOVEFILE_REPLACE_EXISTING) != 0; }

std::string fileDialog(bool save, const char* name) {
    wchar_t buf[MAX_PATH * 4] = {0};
    if (save && name) wcsncpy(buf, wide(name).c_str(), MAX_PATH - 1);
    OPENFILENAMEW o{};
    o.lStructSize = sizeof o;
    o.lpstrFile = buf; o.nMaxFile = (DWORD)(sizeof buf / sizeof *buf);
    o.lpstrFilter = L"All files\0*.*\0";
    o.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const BOOL ok = save ? GetSaveFileNameW(&o) : GetOpenFileNameW(&o);
    return ok ? utf8(buf) : std::string();
}
#else
bool haveProgram(const char* name) {
    const char* path = getenv("PATH");
    if (!path) return false;
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':')) if (!dir.empty() && access((dir + "/" + name).c_str(), X_OK) == 0) return true;
    return false;
}

std::string capture(const std::string& cmd) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {};
    std::string out;
    char buf[512];
    while (fgets(buf, sizeof buf, p)) out += buf;
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

std::string shellQuote(const std::string& s) {
    std::string r = "'";
    for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
    return r + "'";
}

std::string configDir() {
    const char* x = getenv("XDG_CONFIG_HOME");
    if (x && *x) return std::string(x) + "/onair";
    const char* h = getenv("HOME");
    return std::string(h ? h : ".") + "/.config/onair";
}

void makeDirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

bool replaceFile(const std::string& from, const std::string& to) { return rename(from.c_str(), to.c_str()) == 0; }
#endif

std::filesystem::path fsPath(const std::string& p) { return std::filesystem::path(reinterpret_cast<const char8_t*>(p.c_str())); }   // a UTF-8 path

std::string clean(const std::string& s) { std::string r = s; for (char& c : r) if (c == '\n' || c == '\r' || c == '|') c = ' '; return r; }

struct FilePrefs : Prefs {
    std::map<std::string, std::string> kv;
    std::vector<Channel> channels;
    std::string path;
    bool dirty = false;
    FilePrefs() {
        path = configDir() + "/settings.conf";
        std::ifstream f(fsPath(path));
        std::string line;
        int format = 0;
        while (std::getline(f, line)) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
            if (key == prefstext::kFormatKey) {
                format = atoi(val.c_str());
            } else if (key == "channel") {
                std::vector<std::string> p;
                std::stringstream ss(val);
                std::string t;
                while (std::getline(ss, t, '|')) p.push_back(t);
                if (p.size() >= 7) {
                    Channel c;
                    c.freqMhz = atof(p[0].c_str()); c.bwMhz = atof(p[1].c_str()); c.name = p[2]; c.mode = p[3];
                    c.snrDb = (float)atof(p[4].c_str()); c.nServices = atoi(p[5].c_str()); c.favourite = p[6] == "1";
                    if (c.freqMhz > 0) channels.push_back(c);
                }
            } else kv[key] = val;
        }
        for (auto& e : kv) e.second = prefstext::fromFile(e.second, format);
    }
    bool has(const char* key) override { return kv.count(key) != 0; }
    double getD(const char* key, double def) override { auto i = kv.find(key); return i == kv.end() ? def : atof(i->second.c_str()); }
    long getI(const char* key, long def) override { auto i = kv.find(key); return i == kv.end() ? def : atol(i->second.c_str()); }
    bool getB(const char* key, bool def) override { auto i = kv.find(key); return i == kv.end() ? def : i->second == "1"; }
    std::string getS(const char* key, const std::string& def) override { auto i = kv.find(key); return i == kv.end() ? def : i->second; }
    void put(const char* key, const std::string& v) { auto& s = kv[key]; if (s != v) { s = v; dirty = true; } }
    void setD(const char* key, double v) override { char b[64]; snprintf(b, sizeof b, "%.9g", v); put(key, b); }
    void setI(const char* key, long v) override { put(key, std::to_string(v)); }
    void setB(const char* key, bool v) override { put(key, v ? "1" : "0"); }
    void setS(const char* key, const std::string& v) override { put(key, v); }   // kept whole: flush() escapes the line breaks
    std::vector<Channel> getChannels() override { return channels; }
    void setChannels(const std::vector<Channel>& c) override { channels = c; dirty = true; }
    void flush() override {
        if (!dirty) return;
        makeDirs(configDir());
        const std::string tmp = path + ".tmp";
        {
            std::ofstream f(fsPath(tmp));
            if (!f) { fprintf(stderr, "settings: cannot write %s (the changes are kept in memory and tried again)\n", tmp.c_str()); return; }
            f << prefstext::kFormatKey << "=" << prefstext::kFormat << "\n";
            for (auto& e : kv) f << e.first << "=" << prefstext::escape(e.second) << "\n";
            for (auto& c : channels) f << "channel=" << c.freqMhz << "|" << c.bwMhz << "|" << clean(c.name) << "|" << clean(c.mode) << "|" << c.snrDb << "|" << c.nServices << "|" << (c.favourite ? 1 : 0) << "\n";
        }
        if (replaceFile(tmp, path)) { dirty = false; return; }
        // the replace was refused (a virus scanner, a sync client or a locked-folder policy holds the file): write the file in place, so a
        // setting is not silently lost at exit
        std::ofstream f(fsPath(path), std::ios::trunc);
        if (f) {
            f << prefstext::kFormatKey << "=" << prefstext::kFormat << "\n";
            for (auto& e : kv) f << e.first << "=" << prefstext::escape(e.second) << "\n";
            for (auto& c : channels) f << "channel=" << c.freqMhz << "|" << c.bwMhz << "|" << clean(c.name) << "|" << clean(c.mode) << "|" << c.snrDb << "|" << c.nServices << "|" << (c.favourite ? 1 : 0) << "\n";
            f.flush();
            if (f) { dirty = false; return; }
        }
        fprintf(stderr, "settings: cannot replace or write %s (the changes are kept in memory and tried again)\n", path.c_str());
    }
    std::vector<dect2::backup::PrefItem> dumpAll() override {
        std::vector<dect2::backup::PrefItem> o;
        for (auto& e : kv) o.push_back({e.first, 'S', e.second});   // stored as text: getD/getI/getB read it back the same
        return o;
    }
    std::string setAside() override {
        const std::string old = configDir() + "/settings-crashed.conf";
        const bool kept = replaceFile(path, old);
        kv.clear(); channels.clear(); dirty = false;
        return kept ? old : "";
    }
};

} // namespace

Prefs& basePrefs() { static FilePrefs p; return p; }

void showFatalError(const char* title, const char* text) {
#ifdef _WIN32
    MessageBoxA(nullptr, text, title, MB_OK | MB_ICONERROR);
#else
    fprintf(stderr, "%s: %s\n", title, text);
#endif
}

#ifdef _WIN32
std::string openFileDialog() { return fileDialog(false, nullptr); }
std::string saveFileDialog(const char* name) { return fileDialog(true, name); }
std::string openFolderDialog() { return {}; }   // Windows: the path is typed in the export dialog
#else
std::string openFolderDialog() {
    if (haveProgram("zenity")) return capture("zenity --file-selection --directory --title='Choose a folder' 2>/dev/null");
    if (haveProgram("kdialog")) return capture("kdialog --getexistingdirectory \"$HOME\" 2>/dev/null");
    fprintf(stderr, "no folder dialog available (install zenity or kdialog); type the path in the text field instead\n");
    return {};
}

std::string openFileDialog() {
    if (haveProgram("zenity")) return capture("zenity --file-selection --title='Open' 2>/dev/null");
    if (haveProgram("kdialog")) return capture("kdialog --getopenfilename \"$HOME\" 2>/dev/null");
    fprintf(stderr, "no file dialog available (install zenity or kdialog); type the path in the text field instead\n");
    return {};
}

std::string saveFileDialog(const char* name) {
    if (haveProgram("zenity")) return capture("zenity --file-selection --save --confirm-overwrite --filename=" + shellQuote(name) + " 2>/dev/null");
    if (haveProgram("kdialog")) return capture("kdialog --getsavefilename " + shellQuote(name) + " 2>/dev/null");
    fprintf(stderr, "no file dialog available (install zenity or kdialog); type the path in the text field instead\n");
    return {};
}
#endif

#if defined(__linux__)
std::vector<std::string> cjkFontCandidatesFixed();
#endif
std::vector<std::string> uiFontCandidates() {
    return {"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
            "C:/Windows/Fonts/segoeui.ttf"};
}
#if defined(__linux__)
// #336: Fedora, Slackware, Arch and others keep the CJK fonts in folders the fixed list does not name. Ask fontconfig for a font that covers each
// language, then look through the font folders for the usual CJK families.
static void addLinuxCjkFonts(std::vector<std::string>& out) {
    auto add = [&](const std::string& f) { if (!f.empty() && access(f.c_str(), R_OK) == 0 && std::find(out.begin(), out.end(), f) == out.end()) out.push_back(f); };
    for (const char* pat : {":lang=zh-cn", ":lang=zh-tw", ":lang=ja", ":lang=ko"}) {
        std::string cmd = std::string("fc-match -f '%{file}' '") + pat + "' 2>/dev/null";
        if (FILE* p = popen(cmd.c_str(), "r")) {
            char b[1024] = {0};
            if (fgets(b, sizeof b, p)) { std::string f = b; while (!f.empty() && (f.back() == '\n' || f.back() == '\r')) f.pop_back(); add(f); }
            pclose(p);
        }
    }
    namespace fs = std::filesystem;
    std::vector<std::string> roots = {"/usr/share/fonts", "/usr/local/share/fonts"};
    if (const char* h = getenv("HOME")) { roots.push_back(std::string(h) + "/.local/share/fonts"); roots.push_back(std::string(h) + "/.fonts"); }
    for (const std::string& r : roots) {
        std::error_code ec;
        int seen = 0;
        for (fs::recursive_directory_iterator it(r, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end && seen < 20000; it.increment(ec), seen++) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            std::string n = it->path().filename().string();
            std::string l;
            for (char c : n) l += (char)tolower((unsigned char)c);
            const bool font = l.size() > 4 && (l.compare(l.size() - 4, 4, ".ttf") == 0 || l.compare(l.size() - 4, 4, ".ttc") == 0 || l.compare(l.size() - 4, 4, ".otf") == 0);
            if (!font) continue;
            if (l.find("cjk") != std::string::npos || l.find("wqy") != std::string::npos || l.find("sourcehan") != std::string::npos ||
                l.find("droidsansfallback") != std::string::npos || l.find("unifont") != std::string::npos || l.find("arphic") != std::string::npos ||
                l.find("uming") != std::string::npos || l.find("ukai") != std::string::npos || l.find("sarasa") != std::string::npos) {
                if (l.find("bold") != std::string::npos || l.find("black") != std::string::npos || l.find("light") != std::string::npos || l.find("thin") != std::string::npos) continue;
                add(it->path().string());
            }
        }
    }
}
#endif
std::vector<std::string> cjkFontCandidates() {
#if defined(__linux__)
    std::vector<std::string> v = cjkFontCandidatesFixed();
    addLinuxCjkFonts(v);
    return v;
}
std::vector<std::string> cjkFontCandidatesFixed() {
#endif
    return {"C:/Windows/Fonts/malgun.ttf", "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simsun.ttc",
            "C:/Windows/Fonts/YuGothM.ttc", "C:/Windows/Fonts/meiryo.ttc",   // kana and kanji for Japanese if the Chinese fonts are missing
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
            "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc"};
}
std::vector<std::string> monoFontCandidates() {
    return {"C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/lucon.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
            "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf", "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/liberation/LiberationMono-Regular.ttf", "C:/Windows/Fonts/consola.ttf"};
}


std::string uiLanguage() {
#ifdef _WIN32
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_GERMAN: return "de";
        case LANG_FRENCH: return "fr";
        case LANG_SPANISH: return "es";
        case LANG_CHINESE: return "zh";
        case LANG_LITHUANIAN: return "lt";
        case LANG_HUNGARIAN: return "hu";
        case LANG_JAPANESE: return "ja";
        case LANG_POLISH: return "pl";
        case LANG_DUTCH: return "nl";
        case LANG_PORTUGUESE: return SUBLANGID(GetUserDefaultUILanguage()) == SUBLANG_PORTUGUESE ? "pt_PT" : "pt";
        case LANG_ITALIAN: return "it";
        case LANG_RUSSIAN: return "ru";
        default: return "en";
    }
#else
    for (const char* k : {"LC_ALL", "LC_MESSAGES", "LANG"}) if (const char* v = getenv(k)) if (*v && strcmp(v, "C") && strcmp(v, "POSIX")) return v;
    return "en";
#endif
}

}
