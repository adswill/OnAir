// Linux and Windows services. Linux: dialogs through zenity/kdialog, settings in ~/.config/onair/settings.conf.
// Windows: the standard Open/Save dialogs, settings in %APPDATA%\\OnAir\\settings.conf.
#include "platform.h"
#include <cstdio>
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
#endif

namespace plat {

// no picture decoder and no location service on these systems (yet): the map falls back to the plain radar and the position is typed in
bool decodeImage(const std::string&, int&, int&, std::vector<uint32_t>&) { return false; }
std::string cacheDir() { const char* h = getenv("HOME"); std::string d = std::string(h ? h : "/tmp") + "/.cache/onair"; std::string c = "mkdir -p '" + d + "'"; if (system(c.c_str()) != 0) return "/tmp"; return d; }
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

std::string configDir() {
    const char* a = getenv("APPDATA");
    return std::string(a ? a : ".") + "/OnAir";
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

std::string clean(const std::string& s) { std::string r = s; for (char& c : r) if (c == '\n' || c == '\r' || c == '|') c = ' '; return r; }

struct FilePrefs : Prefs {
    std::map<std::string, std::string> kv;
    std::vector<Channel> channels;
    std::string path;
    bool dirty = false;
    FilePrefs() {
        path = configDir() + "/settings.conf";
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
            if (key == "channel") {
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
    void setS(const char* key, const std::string& v) override { put(key, clean(v)); }
    std::vector<Channel> getChannels() override { return channels; }
    void setChannels(const std::vector<Channel>& c) override { channels = c; dirty = true; }
    void flush() override {
        if (!dirty) return;
        makeDirs(configDir());
        const std::string tmp = path + ".tmp";
        {
            std::ofstream f(tmp);
            if (!f) return;
            for (auto& e : kv) f << e.first << "=" << e.second << "\n";
            for (auto& c : channels) f << "channel=" << c.freqMhz << "|" << c.bwMhz << "|" << clean(c.name) << "|" << clean(c.mode) << "|" << c.snrDb << "|" << c.nServices << "|" << (c.favourite ? 1 : 0) << "\n";
        }
        if (replaceFile(tmp, path)) dirty = false;
    }
};

} // namespace

Prefs& prefs() { static FilePrefs p; return p; }

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
#else
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

std::vector<std::string> uiFontCandidates() {
    return {"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
            "C:/Windows/Fonts/segoeui.ttf"};
}
std::vector<std::string> monoFontCandidates() {
    return {"C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/lucon.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
            "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf", "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/liberation/LiberationMono-Regular.ttf", "C:/Windows/Fonts/consola.ttf"};
}

}
