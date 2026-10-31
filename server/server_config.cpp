#include "server_config.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace onair_server {

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}

bool ConfigFile::parse(const std::string& text) {
    lines_.clear();
    std::istringstream in(text);
    std::string l;
    while (std::getline(in, l)) {
        Line x;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        x.raw = l;
        const std::string t = trim(l);
        const size_t eq = t.find('=');
        if (!t.empty() && t[0] != '#' && eq != std::string::npos && eq > 0) {
            x.kv = true;
            x.key = trim(t.substr(0, eq));
            x.value = trim(t.substr(eq + 1));
        }
        lines_.push_back(std::move(x));
    }
    return true;
}

bool ConfigFile::load(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
}

std::string ConfigFile::text() const {
    std::string o;
    for (const Line& l : lines_) { o += l.raw; o += '\n'; }
    return o;
}

bool ConfigFile::save(const std::string& path, std::string& err) const {
    namespace fs = std::filesystem;
    const fs::path p = fs::u8path(path);
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = fs::u8path(path + ".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { err = "cannot write " + path + ".tmp"; return false; }
#ifndef _WIN32
        chmod((path + ".tmp").c_str(), 0600);   // before the token goes in
#endif
        const std::string t = text();
        f.write(t.data(), (std::streamsize)t.size());
        if (!f) { err = "cannot write " + path + ".tmp"; return false; }
    }
    fs::rename(tmp, p, ec);
    if (ec) { err = "cannot replace " + path + ": " + ec.message(); fs::remove(tmp, ec); return false; }
    return true;
}

bool writeHostFile(const std::string& path, const std::string& data, std::string& err, bool atomic) {
    namespace fs = std::filesystem;
    const std::string to = atomic ? path + ".tmp" : path;
    {
        std::ofstream f(fs::u8path(to), std::ios::binary | std::ios::trunc);
        if (f) f.write(data.data(), (std::streamsize)data.size());
        if (!f) { err = "cannot write " + to; return false; }
    }
    if (atomic) {
        std::error_code ec;
        fs::rename(fs::u8path(to), fs::u8path(path), ec);
        if (ec) { err = "cannot replace " + path + ": " + ec.message(); fs::remove(fs::u8path(to), ec); return false; }
    }
    return true;
}

void removeHostFile(const std::string& path) { std::error_code ec; std::filesystem::remove(std::filesystem::u8path(path), ec); }

std::string ConfigFile::get(const std::string& key, const std::string& def) const {
    for (size_t i = lines_.size(); i-- > 0;) if (lines_[i].kv && lines_[i].key == key) return lines_[i].value;
    return def;
}

std::vector<std::string> ConfigFile::getAll(const std::string& key) const {
    std::vector<std::string> v;
    for (const Line& l : lines_) if (l.kv && l.key == key) v.push_back(l.value);
    return v;
}

bool ConfigFile::has(const std::string& key) const {
    for (const Line& l : lines_) if (l.kv && l.key == key) return true;
    return false;
}

double ConfigFile::num(const std::string& key, double def) const {
    const std::string v = get(key);
    if (v.empty()) return def;
    char* e = nullptr;
    const double d = strtod(v.c_str(), &e);
    return e && e != v.c_str() && *e == 0 ? d : def;
}

bool ConfigFile::flag(const std::string& key, bool def) const {
    const std::string v = get(key);
    if (v.empty()) return def;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

void ConfigFile::set(const std::string& key, const std::string& value) {
    bool placed = false;
    for (size_t i = 0; i < lines_.size();) {
        Line& l = lines_[i];
        if (l.kv && l.key == key) {
            if (!placed) { l.value = value; l.raw = key + "=" + value; placed = true; i++; }
            else lines_.erase(lines_.begin() + (long)i);
        } else i++;
    }
    if (!placed) { Line l; l.kv = true; l.key = key; l.value = value; l.raw = key + "=" + value; lines_.push_back(std::move(l)); }
}

void ConfigFile::setNum(const std::string& key, double v) {
    char b[48];
    snprintf(b, sizeof b, "%.10g", v);
    set(key, b);
}

std::string defaultConfigPath() {
    if (const char* e = getenv("ONAIR_SERVER_CONFIG")) if (*e) return e;
#ifdef _WIN32
    if (const char* a = getenv("APPDATA")) return std::string(a) + "\\OnAir\\server.conf";
    return "server.conf";
#else
    if (const char* x = getenv("XDG_CONFIG_HOME")) if (*x) return std::string(x) + "/onair/server.conf";
    if (const char* h = getenv("HOME")) return std::string(h) + "/.config/onair/server.conf";
    return "/etc/onair/server.conf";
#endif
}

} // namespace onair_server
